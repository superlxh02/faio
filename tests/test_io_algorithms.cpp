#include "backend_test_support.hpp"
/**
 * @file test_io_algorithms.cpp
 * @brief 与系统后端无关的 IO 算法契约：短 IO、进度错误、有限容量、整数编码。
 * @details mock 只有公共 read/write 接口，用编译实例化同时验证 concept
 * 的可组合性。
 */
#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {
using faio_test::take;

/** @brief 固定短读 reader；可在累计指定进度后失败，也可模拟违反接口的结果。 */
struct fragment_reader {
  std::string input;
  std::size_t cursor{};
  std::size_t fragment{3};
  std::size_t failure_after{std::string::npos};
  bool invalid_result{};
  faio::task<faio::expected<std::size_t>> read(std::span<char> output) {
    if (invalid_result)
      co_return output.size() + 1;
    if (cursor >= failure_after)
      co_return std::unexpected{faio::Error{EIO, 0}};
    const auto amount =
        std::min({fragment, output.size(), input.size() - cursor,
                  failure_after - cursor});
    if (amount)
      std::memcpy(output.data(), input.data() + cursor, amount);
    cursor += amount;
    co_return amount;
  }
};

/** @brief 固定短写 writer；有限失败点与零写用于验证 write_all 的退出条件。 */
struct fragment_writer {
  std::string output;
  std::size_t fragment{2};
  std::size_t failure_after{std::string::npos};
  bool zero_progress{};
  faio::task<faio::expected<std::size_t>> write(std::span<const char> input) {
    if (zero_progress)
      co_return 0;
    if (output.size() >= failure_after)
      co_return std::unexpected{faio::Error{EIO, 0}};
    const auto amount =
        std::min({fragment, input.size(), failure_after - output.size()});
    output.append(input.data(), amount);
    co_return amount;
  }
};
/** @brief 单次失败也可能已经生效两个字节，Error::progress 必须参与后续重试。 */
struct progress_error_writer {
  std::string output;
  bool fail{true};
  faio::task<faio::expected<std::size_t>> write(std::span<const char> input) {
    if (fail) {
      const auto count = std::min(std::size_t{2}, input.size());
      output.append(input.data(), count);
      co_return std::unexpected{faio::Error{EIO, count}};
    }
    output.append(input.data(), input.size());
    co_return input.size();
  }
};
static_assert(faio::io::borrowed_async_reader<fragment_reader>);
static_assert(faio::io::borrowed_async_writer<fragment_writer>);

faio::task<bool> exact_and_all() {
  fragment_reader reader{"短 IO 完整读取"};
  std::vector<char> output(reader.input.size());
  if (take(co_await faio::io::read_exact(reader, output)) != output.size())
    co_return false;
  if (std::string(output.data(), output.size()) != reader.input)
    co_return false;
  fragment_writer writer;
  take(co_await faio::io::write_all(writer, std::span<const char>{output}));
  if (writer.output != reader.input)
    co_return false;
  auto owned = take(co_await faio::io::write_all_buf(
      writer, faio::io::io_buffer::copy(std::string_view{"owned"})));
  co_return owned.bytes == 5 && owned.buffer.size() == 5 &&
      writer.output == reader.input + "owned";
}

/** @brief 组合错误必须携带此前已生效的进度，不得吞掉 EOF 或无限重试零写。 */
faio::task<bool> partial_errors() {
  std::array<char, 8> output{};
  fragment_reader eof{"12345"};
  auto early = co_await faio::io::read_exact(eof, output);
  if (early || early.error().value() != faio::Error::UnexpectedEOF ||
      early.error().progress() != 5)
    co_return false;
  fragment_reader failure{"abcdefgh", 0, 3, 4};
  auto read = co_await faio::io::read_exact(failure, output);
  if (read || read.error().value() != EIO || read.error().progress() != 4)
    co_return false;
  fragment_writer writer{"", 2, 3};
  auto write = co_await faio::io::write_all(writer, std::string_view{"abcdef"});
  if (write || write.error().value() != EIO || write.error().progress() != 3 ||
      writer.output != "abc")
    co_return false;
  fragment_writer zero{"", 2, std::string::npos, true};
  auto stalled = co_await faio::io::write_all(zero, std::string_view{"x"});
  co_return !stalled && stalled.error().value() == faio::Error::WriteZero;
}

/** @brief 本次追加量受到 max_bytes 约束；已有前缀以及未读输入必须保留。 */
faio::task<bool> read_limits_and_copy() {
  fragment_reader source{"abcdefgh"};
  std::vector<char> bytes{'p'};
  if (take(co_await faio::io::read_to_end(source, bytes, 5)) != 5 ||
      bytes != std::vector<char>{'p', 'a', 'b', 'c', 'd', 'e'})
    co_return false;
  std::string tail = "prefix:";
  if (take(co_await faio::io::read_to_string(source, tail)) != 3 ||
      tail != "prefix:fgh")
    co_return false;
  fragment_reader copied{"abcdefghijklmnopqrstuvwxyz", 0, 3};
  fragment_writer destination;
  if (take(co_await faio::io::copy(copied, destination, 4)) !=
          copied.input.size() ||
      destination.output != copied.input)
    co_return false;
  auto invalid_capacity = co_await faio::io::copy(copied, destination, 0);
  if (invalid_capacity || invalid_capacity.error().value() != EINVAL)
    co_return false;
  fragment_reader invalid{"", 0, 3, std::string::npos, true};
  auto overreported = co_await faio::io::read_to_end(invalid, bytes, 3);
  co_return !overreported && overreported.error().value() == EIO;
}

/** @brief 大小端 8/16/32/64 位及有符号编码都跨多个短 IO。 */
faio::task<bool> integer_roundtrip() {
  fragment_writer writer;
  take(co_await faio::io::write_u8(writer, 0x7fu));
  take(co_await faio::io::write_u16(writer, 0x1234u));
  take(co_await faio::io::write_u32(writer, 0x12345678u));
  take(co_await faio::io::write_u64(writer, 0x123456789abcdef0ULL));
  take(co_await faio::io::write_i8(writer, -7));
  take(co_await faio::io::write_i16_le(writer, -1234));
  take(co_await faio::io::write_i32_le(writer, -1234567));
  take(co_await faio::io::write_i64_le(writer, -1234567890LL));
  if (static_cast<unsigned char>(writer.output[1]) != 0x12u ||
      static_cast<unsigned char>(writer.output[2]) != 0x34u)
    co_return false;
  fragment_reader reader{writer.output};
  co_return take(co_await faio::io::read_u8(reader)) == 0x7fu &&
      take(co_await faio::io::read_u16(reader)) == 0x1234u &&
      take(co_await faio::io::read_u32(reader)) == 0x12345678u &&
      take(co_await faio::io::read_u64(reader)) == 0x123456789abcdef0ULL &&
      take(co_await faio::io::read_i8(reader)) == -7 &&
      take(co_await faio::io::read_i16_le(reader)) == -1234 &&
      take(co_await faio::io::read_i32_le(reader)) == -1234567 &&
      take(co_await faio::io::read_i64_le(reader)) == -1234567890LL;
}

/** @brief 预读不丢尾部、consume 边界、分隔符/CRLF/空行/EOF 与单条大小上限。 */
faio::task<bool> buffered_reader_and_records() {
  fragment_reader source{"alpha\r\n\nbeta\nlast", 0, 64};
  faio::io::BufReader buffer{source, 8};
  auto available = take(co_await buffer.fill_buf());
  if (available.size() != 8 ||
      std::string_view(available.data(), 7) != "alpha\r\n")
    co_return false;
  auto records = faio::io::lines(buffer, 32);
  auto first = take(co_await records.next_line());
  auto empty = take(co_await records.next_line());
  auto second = take(co_await records.next_line());
  auto last = take(co_await records.next_line());
  auto eof = take(co_await records.next_line());
  if (!first || *first != "alpha" || !empty || !empty->empty() || !second ||
      *second != "beta" || !last || *last != "last" || eof)
    co_return false;
  fragment_reader delimited{"a|b||tail", 0, 64};
  faio::io::BufReader buffered{delimited, 3};
  auto segments = faio::io::split_by_delimiter(buffered, '|');
  for (const auto expected : {std::string_view{"a"}, std::string_view{"b"},
                              std::string_view{}, std::string_view{"tail"}}) {
    auto segment = take(co_await segments.next_segment());
    if (!segment ||
        std::string_view(segment->data(), segment->size()) != expected)
      co_return false;
  }
  if (take(co_await segments.next_segment()))
    co_return false;
  fragment_reader long_line{"abcdef\n", 0, 64};
  faio::io::BufReader limited{long_line, 8};
  std::vector<char> output;
  auto limit = co_await faio::io::read_until(limited, '\n', output, 3);
  if (limit || limit.error().value() != EMSGSIZE ||
      limit.error().progress() != 3 || output.size() != 3)
    co_return false;
  if (std::string_view(limited.buffered().data(), limited.buffered().size()) !=
      "def\n")
    co_return false;
  co_return true;
}

/** @brief 显式 flush 提交，部分写失败保留未提交尾部，恢复后不重复已有前缀。 */
faio::task<bool> buffered_writer_retains_pending() {
  fragment_writer destination{"", 2, 3};
  faio::io::BufWriter writer{destination, 16};
  if (take(co_await writer.write(std::span<const char>{"abcdefgh", 8})) != 8 ||
      !destination.output.empty())
    co_return false;
  auto failed = co_await writer.flush();
  if (failed || failed.error().progress() != 3 || destination.output != "abc" ||
      std::string_view(writer.pending_bytes().data(),
                       writer.pending_bytes().size()) != "defgh")
    co_return false;
  destination.failure_after = std::string::npos;
  take(co_await writer.flush());
  if (destination.output != "abcdefgh" || !writer.pending_bytes().empty())
    co_return false;
  take(co_await writer.write(std::span<const char>{"pending", 7}));
  auto pending = writer.take_pending();
  if (std::string_view(pending.data(), pending.size()) != "pending" ||
      !writer.pending_bytes().empty())
    co_return false;
  take(co_await writer.shutdown());
  progress_error_writer progressed;
  faio::io::BufWriter progress_buffer{progressed, 16};
  take(co_await progress_buffer.write(std::span<const char>{"abcd", 4}));
  const auto partial = co_await progress_buffer.flush();
  if (partial || partial.error().progress() != 2 ||
      std::string_view(progress_buffer.pending_bytes().data(),
                       progress_buffer.pending_bytes().size()) != "cd")
    co_return false;
  progressed.fail = false;
  take(co_await progress_buffer.flush());
  if (progressed.output != "abcd")
    co_return false;
  co_return true;
}

faio::task<bool> memory_adapters_and_bufstream() {
  faio::io::MemoryStream first{std::string_view{"abcdef"}};
  auto limited = faio::io::take(first, 3);
  std::array<char, 8> bytes{};
  if (take(co_await limited.read(bytes)) != 3 || limited.limit() != 0 ||
      take(co_await limited.read(bytes)) != 0)
    co_return false;
  if (take(co_await first.read(bytes)) != 3 ||
      std::string_view(bytes.data(), 3) != "def")
    co_return false;
  faio::io::MemoryStream left{std::string_view{"ab"}},
      right{std::string_view{"cd"}};
  auto combined = faio::io::chain(left, right);
  if (take(co_await combined.read({})) != 0)
    co_return false;
  std::string output;
  if (take(co_await faio::io::read_to_string(combined, output)) != 4 ||
      output != "abcd")
    co_return false;
  auto repeating = faio::io::repeat('r');
  auto bounded = faio::io::take(repeating, 11);
  auto sink = faio::io::sink();
  if (take(co_await faio::io::copy(bounded, sink, 4)) != 11)
    co_return false;
  auto empty = faio::io::empty();
  if (take(co_await empty.read(bytes)) != 0)
    co_return false;
  faio::io::MemoryStream stream;
  faio::io::BufStream buffered{stream, 4, 8};
  take(co_await faio::io::write_all(buffered, std::string_view{"bufstream"}));
  take(co_await buffered.flush());
  take(co_await stream.seek(0));
  output.clear();
  take(co_await faio::io::read_to_string(buffered, output));
  co_return output == "bufstream";
}

faio::task<void> duplex_writer(faio::io::DuplexStream &stream,
                               const std::string &message) {
  take(co_await faio::io::write_all(stream, std::string_view{message}));
  take(co_await stream.shutdown_write());
}
/** @brief 有界 duplex 持续写满会挂起，读者排空后继续，half close 后正确 EOF。
 */
faio::task<bool> duplex_backpressure_and_halfclose() {
  auto endpoints = faio::io::duplex(32);
  const std::string payload(4096, 'd');
  auto writer = faio::spawn(duplex_writer(endpoints.first, payload));
  std::string output;
  take(co_await faio::io::read_to_string(endpoints.second, output));
  co_await writer;
  if (output != payload)
    co_return false;
  take(co_await faio::io::write_all(endpoints.second,
                                    std::string_view{"reply"}));
  std::array<char, 5> bytes{};
  take(co_await faio::io::read_exact(endpoints.first, bytes));
  co_return std::string_view(bytes.data(), bytes.size()) == "reply";
}
} // namespace

TEST(IoBufferContract, MoveTransfersInitializedRangeAndGrowthZeroInitializes) {
  auto first = faio::io::io_buffer::copy(std::string_view{"abc"});
  auto second = std::move(first);
  EXPECT_EQ(first.capacity(), 0u);
  EXPECT_EQ(first.size(), 0u);
  second.resize(6);
  EXPECT_EQ(std::string_view(second.data(), 3), "abc");
  EXPECT_EQ(std::string_view(second.data() + 3, 3),
            std::string_view("\0\0\0", 3));
  EXPECT_THROW(second.set_size(second.capacity() + 1), std::out_of_range);
  std::array<char, 4> memory{};
  faio::io::read_buf buffer{memory, 1};
  EXPECT_EQ(buffer.unfilled().size(), 3u);
  buffer.advance(3);
  EXPECT_EQ(buffer.filled().size(), 4u);
  EXPECT_THROW(buffer.advance(1), std::out_of_range);
  buffer.clear();
  EXPECT_EQ(buffer.size(), 0u);
}

TEST(IoAlgorithmContract, ExactAndAllLoopOverPartialTransfers) {
  faio_test::runtime_context runtime{
      faio_test::config_builder()
          .set_mode(faio::runtime::mode::current_thread)
          .build()};
  EXPECT_TRUE(runtime.block_on(exact_and_all()));
}
TEST(IoAlgorithmContract, EofWriteZeroAndFailuresPreserveProgress) {
  faio_test::runtime_context runtime{
      faio_test::config_builder()
          .set_mode(faio::runtime::mode::current_thread)
          .build()};
  EXPECT_TRUE(runtime.block_on(partial_errors()));
}
TEST(IoAlgorithmContract, ReadLimitCopyAndInvalidProviderResult) {
  faio_test::runtime_context runtime{
      faio_test::config_builder()
          .set_mode(faio::runtime::mode::current_thread)
          .build()};
  EXPECT_TRUE(runtime.block_on(read_limits_and_copy()));
}
TEST(IoAlgorithmContract, IntegerEncodingWorksWithFragmentedIo) {
  faio_test::runtime_context runtime{
      faio_test::config_builder()
          .set_mode(faio::runtime::mode::current_thread)
          .build()};
  EXPECT_TRUE(runtime.block_on(integer_roundtrip()));
}
TEST(IoAlgorithmContract, BufReaderDelimitedRecordsAndLineLimits) {
  faio_test::runtime_context runtime{
      faio_test::config_builder()
          .set_mode(faio::runtime::mode::current_thread)
          .build()};
  EXPECT_TRUE(runtime.block_on(buffered_reader_and_records()));
}
TEST(IoAlgorithmContract, BufWriterRetainsUnsubmittedTailAfterPartialFailure) {
  faio_test::runtime_context runtime{
      faio_test::config_builder()
          .set_mode(faio::runtime::mode::current_thread)
          .build()};
  EXPECT_TRUE(runtime.block_on(buffered_writer_retains_pending()));
}
TEST(IoAlgorithmContract, MemoryTakeChainRepeatSinkAndBufStream) {
  faio_test::runtime_context runtime{
      faio_test::config_builder()
          .set_mode(faio::runtime::mode::current_thread)
          .build()};
  EXPECT_TRUE(runtime.block_on(memory_adapters_and_bufstream()));
}
TEST(IoAlgorithmContract, DuplexBoundedBackpressureAndIndependentHalfClose) {
  faio_test::runtime_context runtime{
      faio_test::config_builder().set_num_workers(4).build()};
  EXPECT_TRUE(runtime.block_on(duplex_backpressure_and_halfclose()));
}
