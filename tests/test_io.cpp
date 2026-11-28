/**
 * @file test_io.cpp
 * @brief 内存流、组合读写、文件与错误传播的单元测试。
 */
#include "test_helpers.hpp"
#include "faio/detail/io/stream/endian.hpp"
#include <gtest/gtest.h>
#include <array>
#include <string_view>

namespace {
/** @brief 验证内存流的完整读写、游标及整数编码。 */
faio::task<bool> memory_roundtrip() {
  faio::io::memory_stream stream;
  faio_test::take(co_await faio::io::write_u32(stream, 0x12345678u));
  faio_test::take(co_await faio::io::write_all(stream, std::string_view{"faio"}));
  faio_test::take(co_await stream.seek(0));
  if (faio_test::take(co_await faio::io::read_u32(stream)) != 0x12345678u)
    co_return false;
  std::array<char, 4> bytes{};
  faio_test::take(co_await faio::io::read_exact(stream, bytes));
  co_return std::string_view(bytes.data(), bytes.size()) == "faio";
}

/** @brief EOF 错误携带此前已完成的读取进度。 */
faio::task<bool> eof_progress() {
  faio::io::memory_stream stream{std::string_view{"abc"}};
  std::array<char, 5> output{};
  const auto result = co_await faio::io::read_exact(stream, output);
  co_return !result && result.error().value() == faio::error::UNEXPECTED_EOF
      && result.error().progress() == 3;
}

/** @brief 验证普通文件的完整读写、定位与元数据。 */
faio::task<bool> file_roundtrip(const std::filesystem::path& path) {
  auto file = faio_test::take(
      co_await faio::fs::file::open(path, faio::fs::open_options{}.read().write().create_new()));
  constexpr std::string_view payload{"faio-file"};
  faio_test::take(co_await file.write_all(std::span<const char>{payload}));
  faio_test::take(co_await file.flush());
  const auto metadata = faio_test::take(co_await file.metadata());
  if (!metadata.is_file() || metadata.len() != payload.size())
    co_return false;
  faio_test::take(co_await file.seek(0));
  std::array<char, 9> bytes{};
  faio_test::take(co_await file.read_exact(bytes));
  faio_test::take(co_await file.close());
  co_return std::string_view(bytes.data(), bytes.size()) == payload;
}

/** @brief 打开不存在的文件时返回错误。 */
faio::task<bool> missing_file(const std::filesystem::path& path) {
  const auto file = co_await faio::fs::file::open(path, faio::fs::open_options{}.read());
  co_return !file;
}
}  // namespace

/** @brief 内存流支持组合读写与整数编码。 */
TEST(Io, memory_stream) {
  faio::runtime::detail::runtime_context context{faio_test::config()};
  EXPECT_TRUE(context.block_on(memory_roundtrip()));
}

/** @brief 读取不足指定长度时报告 EOF 和已读取的字节数。 */
TEST(Io, PartialReadError) {
  faio::runtime::detail::runtime_context context{faio_test::config()};
  EXPECT_TRUE(context.block_on(eof_progress()));
}

/** @brief 文件 API 通过当前平台后端读写字节。 */
TEST(Io, Filesystem) {
  faio_test::temporary_directory directory;
  faio::runtime::detail::runtime_context context{faio_test::config()};
  EXPECT_TRUE(context.block_on(file_roundtrip(directory.path() / "content")));
  EXPECT_TRUE(context.block_on(missing_file(directory.path() / "missing")));
}

/** @brief 错误副本保持错误域、上下文和字节进度。 */
TEST(Io, ErrorContext) {
  auto context = std::make_shared<faio::error_context>();
  context->message = "测试错误";
  const auto error = faio::error{3, 7, faio::error_domain::TLS}.with_context(context);
  const auto copy = error;
  EXPECT_EQ(copy.domain(), faio::error_domain::TLS);
  EXPECT_EQ(copy.progress(), 7u);
  EXPECT_EQ(copy.context(), context);
  EXPECT_EQ(copy.message(), "测试错误");
}
