/**
 * @file test_stdio.cpp
 * @brief 标准流的所有权、缓冲、并发记录及取消行为。
 */
#include "test_helpers.hpp"
#include <faio/stdio/stream.hpp>
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
/** @brief 测试内临时替换标准描述符，退出时始终恢复原描述符。 */
class stream_redirect {
 public:
  explicit stream_redirect(std::FILE* stream, std::string_view input = {})
      : stream_(stream), file_(std::tmpfile()) {
    if (!file_)
      throw std::runtime_error("创建标准流测试文件失败");
    if (!input.empty() && std::fwrite(input.data(), 1, input.size(), file_) != input.size()) {
      std::fclose(file_);
      throw std::runtime_error("写入标准流测试输入失败");
    }
    std::rewind(file_);
    std::fflush(stream_);
#if defined(_WIN32)
    destination_ = ::_fileno(stream_);
    previous_ = ::_dup(destination_);
    const auto replaced = previous_ < 0 ? -1 : ::_dup2(::_fileno(file_), destination_);
#else
    destination_ = ::fileno(stream_);
    previous_ = ::dup(destination_);
    const auto replaced = previous_ < 0 ? -1 : ::dup2(::fileno(file_), destination_);
#endif
    if (previous_ < 0 || replaced < 0) {
      if (previous_ >= 0)
        close_descriptor(previous_);
      std::fclose(file_);
      throw std::runtime_error("重定向标准流失败");
    }
    std::clearerr(stream_);
  }

  stream_redirect(const stream_redirect&) = delete;
  stream_redirect& operator=(const stream_redirect&) = delete;

  ~stream_redirect() {
    std::fflush(stream_);
#if defined(_WIN32)
    (void)::_dup2(previous_, destination_);
#else
    (void)::dup2(previous_, destination_);
#endif
    close_descriptor(previous_);
    std::clearerr(stream_);
    std::fclose(file_);
  }

  auto contents() -> std::string {
    std::fflush(stream_);
    std::rewind(file_);
    std::string result;
    std::array<char, 4096> bytes{};
    while (const auto count = std::fread(bytes.data(), 1, bytes.size(), file_))
      result.append(bytes.data(), count);
    return result;
  }

 private:
  static void close_descriptor(int descriptor) noexcept {
#if defined(_WIN32)
    (void)::_close(descriptor);
#else
    (void)::close(descriptor);
#endif
  }

  std::FILE* stream_;
  std::FILE* file_;
  int destination_{};
  int previous_{-1};
};

/** @brief 已创建的输出任务拥有临时文本，标准流门面不必继续存活。 */
faio::task<bool> write_temporary() {
  auto pending = faio::stdio::out().write_all(std::string{"owned temporary"});
  faio_test::take(co_await std::move(pending));
  faio_test::take(co_await faio::stdio::out().write_all("\n"));
  auto buffer = faio::io::io_buffer::copy(std::string_view{"buffer"});
  const auto transfer = faio_test::take(co_await faio::stdio::out().write(std::move(buffer)));
  faio_test::take(co_await faio::stdio::out().flush());
  co_return transfer.bytes == 6 && transfer.buffer.size() == 6;
}

/** @brief read_line 与 read 共享预读字节，并区分空行、末行和 EOF。 */
faio::task<bool> mixed_reads() {
  auto first_task = faio::stdio::in().read_line();
  const auto first = faio_test::take(co_await std::move(first_task));
  const auto empty = faio_test::take(co_await faio::stdio::in().read_line());
  std::array<char, 2> prefix{};
  const auto count = faio_test::take(co_await faio::stdio::in().read(prefix));
  const auto suffix = faio_test::take(co_await faio::stdio::in().read(faio::io::io_buffer{8}));
  const auto eof = faio_test::take(co_await faio::stdio::in().read_line());
  co_return first == "first\n" && empty == "\n" && count == 2
      && std::string_view{prefix.data(), prefix.size()} == "la" && suffix.bytes == 2
      && std::string_view{suffix.buffer.data(), suffix.bytes} == "st" && !eof;
}

/** @brief 已取消或已超时的操作不提交输出。 */
faio::task<bool> cancelled_output() {
  std::stop_source stop;
  stop.request_stop();
  faio::io::operation_options cancelled;
  cancelled.stop = stop.get_token();
  const auto first = co_await faio::stdio::out().write_all("cancelled", cancelled);
  faio::io::operation_options expired;
  expired.deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1};
  const auto second = co_await faio::stdio::out().write_all("expired", expired);
  co_return !first && first.error().value() == ECANCELED && !second
      && second.error().value() == ETIMEDOUT;
}

/** @brief 长度上限立即报错，剩余输入仍保存在共享预读缓冲中。 */
faio::task<bool> limited_lines() {
  const auto zero = co_await faio::stdio::in().read_line(0);
  const auto limited = co_await faio::stdio::in().read_line(3);
  const auto rest = faio_test::take(co_await faio::stdio::in().read_line());
  const auto eof = faio_test::take(co_await faio::stdio::in().read_line());
  co_return !zero && zero.error().value() == EINVAL && !limited
      && limited.error().value() == EMSGSIZE && limited.error().progress() == 3 && rest == "def\n"
      && !eof;
}

/** @brief 同一输出通道中的并发记录不会逐段交叉。 */
faio::task<void> concurrent_records() {
  std::vector<faio::task<faio::expected<void>>> records;
  for (unsigned index = 0; index != 16; ++index) {
    std::string message(8192, static_cast<char>('a' + index));
    message += '\n';
    records.push_back(faio::stdio::out().write_all(std::move(message)));
  }
  const auto results = co_await faio::join_all(std::move(records));
  for (const auto& result : results)
    if (!result)
      throw std::runtime_error("并发标准输出写入失败");
}
}  // namespace

TEST(Stdio, TemporaryStreamAndOwnedBuffers) {
  bool correct{};
  std::string contents;
  {
    stream_redirect output{stdout};
    faio::runtime::detail::runtime_context runtime{faio_test::config()};
    correct = runtime.block_on(write_temporary());
    contents = output.contents();
  }
  EXPECT_TRUE(correct);
  EXPECT_EQ(contents, "owned temporary\nbuffer");
}

TEST(Stdio, BufferedInputAndEof) {
  bool correct{};
  {
    stream_redirect input{stdin, "first\n\nlast"};
    faio::runtime::detail::runtime_context runtime{faio_test::config()};
    correct = runtime.block_on(mixed_reads());
  }
  EXPECT_TRUE(correct);
}

TEST(Stdio, CancelledAndExpiredOutputDoesNotWrite) {
  bool correct{};
  std::string contents;
  {
    stream_redirect output{stdout};
    faio::runtime::detail::runtime_context runtime{faio_test::config()};
    correct = runtime.block_on(cancelled_output());
    contents = output.contents();
  }
  EXPECT_TRUE(correct);
  EXPECT_TRUE(contents.empty());
}

TEST(Stdio, LineLengthLimitRetainsRemainingInput) {
  bool correct{};
  {
    stream_redirect input{stdin, "abcdef\n"};
    faio::runtime::detail::runtime_context runtime{faio_test::config()};
    correct = runtime.block_on(limited_lines());
  }
  EXPECT_TRUE(correct);
}

TEST(Stdio, ConcurrentRecordsAreWhole) {
  std::string contents;
  {
    stream_redirect output{stdout};
    faio::runtime::detail::runtime_context runtime{faio_test::config()};
    runtime.block_on(concurrent_records());
    contents = output.contents();
  }
  ASSERT_EQ(contents.size(), 16u * 8193);
  std::array<bool, 16> seen{};
  for (std::size_t offset = 0; offset != contents.size(); offset += 8193) {
    const auto letter = contents[offset];
    ASSERT_GE(letter, 'a');
    ASSERT_LE(letter, 'p');
    const auto index = static_cast<unsigned>(letter - 'a');
    EXPECT_FALSE(seen[index]);
    seen[index] = true;
    EXPECT_EQ(contents.substr(offset, 8192), std::string(8192, letter));
    EXPECT_EQ(contents[offset + 8192], '\n');
  }
}

TEST(Stdio, StandardErrorUsesItsOwnChannel) {
  std::string contents;
  {
    stream_redirect output{stderr};
    faio::runtime::detail::runtime_context runtime{faio_test::config()};
    faio_test::take(runtime.block_on(faio::stdio::err().write_all("standard error")));
    contents = output.contents();
  }
  EXPECT_EQ(contents, "standard error");
}

#if !defined(_WIN32)
namespace {
/** @brief 保存并恢复一个进程标准描述符。 */
class descriptor_restore {
 public:
  explicit descriptor_restore(int descriptor) : descriptor_(descriptor), saved_(::dup(descriptor)) {
    if (saved_ < 0)
      throw std::runtime_error("保存标准描述符失败");
  }

  ~descriptor_restore() {
    (void)::dup2(saved_, descriptor_);
    (void)::close(saved_);
  }

  descriptor_restore(const descriptor_restore&) = delete;
  descriptor_restore& operator=(const descriptor_restore&) = delete;

 private:
  int descriptor_;
  int saved_;
};

/** @brief 读取挂起期间，当前线程运行时仍能推进定时器和其他任务。 */
faio::task<bool> delayed_input(int write_descriptor, bool& timer_ran) {
  co_await faio::time::sleep(std::chrono::milliseconds{5});
  timer_ran = true;
  co_return ::write(write_descriptor, "x", 1) == 1;
}

faio::task<bool> read_without_blocking_scheduler(int write_descriptor) {
  bool timer_ran{};
  std::array<char, 16> bytes{};
  auto [read, fed] = co_await faio::join(faio::stdio::in().read(bytes),
                                         delayed_input(write_descriptor, timer_ran));
  co_return read && *read == 1 && bytes[0] == 'x' && fed&& timer_ran;
}
}  // namespace

TEST(Stdio, BlockingReadDoesNotBlockSchedulerOrChangeFlags) {
  int descriptors[2];
  ASSERT_EQ(::pipe(descriptors), 0);
  bool correct{};
  int before{};
  int after{};
  {
    descriptor_restore restore{STDIN_FILENO};
    ASSERT_GE(::dup2(descriptors[0], STDIN_FILENO), 0);
    before = ::fcntl(STDIN_FILENO, F_GETFL, 0);
    faio::runtime::detail::runtime_context runtime{
        faio_test::config(faio::runtime::mode::CURRENT_THREAD)};
    correct = runtime.block_on(read_without_blocking_scheduler(descriptors[1]));
    after = ::fcntl(STDIN_FILENO, F_GETFL, 0);
  }
  ::close(descriptors[0]);
  ::close(descriptors[1]);
  EXPECT_TRUE(correct);
  EXPECT_EQ(before, after);
}

TEST(Stdio, BrokenPipeReturnsErrorWithoutSignalTermination) {
  int descriptors[2];
  ASSERT_EQ(::pipe(descriptors), 0);
  faio::expected<void> result;
  {
    std::fflush(stdout);
    descriptor_restore restore{STDOUT_FILENO};
    ASSERT_GE(::dup2(descriptors[1], STDOUT_FILENO), 0);
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    faio::runtime::detail::runtime_context runtime{faio_test::config()};
    result = runtime.block_on(faio::stdio::out().write_all("broken pipe"));
  }
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().value(), EPIPE);
}

TEST(Stdio, RunningWriteDrainsAndReportsCancelledProgress) {
  int descriptors[2];
  ASSERT_EQ(::pipe(descriptors), 0);
  constexpr std::size_t message_size = 1024 * 1024;
  std::size_t received{};
  faio::expected<void> result;
  {
    std::fflush(stdout);
    descriptor_restore restore{STDOUT_FILENO};
    ASSERT_GE(::dup2(descriptors[1], STDOUT_FILENO), 0);
    ::close(descriptors[1]);
    std::stop_source stop;
    std::jthread reader{[&] {
      std::array<char, 4096> bytes{};
      while (received != message_size) {
        const auto count = ::read(descriptors[0], bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR)
          continue;
        if (count <= 0)
          return;
        received += static_cast<std::size_t>(count);
        // 收到首批字节证明系统 write 已经开始；取消后继续排空以避免借用内存悬空。
        stop.request_stop();
      }
    }};
    faio::io::operation_options options;
    options.stop = stop.get_token();
    faio::runtime::detail::runtime_context runtime{faio_test::config()};
    result =
        runtime.block_on(faio::stdio::out().write_all(std::string(message_size, 'x'), options));
  }
  ::close(descriptors[0]);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().value(), ECANCELED);
  EXPECT_EQ(result.error().progress(), message_size);
  EXPECT_EQ(received, message_size);
}

#if defined(F_SETNOSIGPIPE) && defined(F_GETNOSIGPIPE)
TEST(Stdio, NativeSignalFlagIsRestoredAfterSuccessfulOutput) {
  std::array<int, 2> after{};
  {
    stream_redirect output{stdout};
    faio::runtime::detail::runtime_context runtime{faio_test::config()};
    for (int enabled = 0; enabled != 2; ++enabled) {
      ASSERT_EQ(::fcntl(STDOUT_FILENO, F_SETNOSIGPIPE, enabled), 0);
      faio_test::take(runtime.block_on(faio::stdio::out().write_all("success")));
      after[static_cast<std::size_t>(enabled)] = ::fcntl(STDOUT_FILENO, F_GETNOSIGPIPE);
    }
  }
  EXPECT_EQ(after[0], 0);
  EXPECT_EQ(after[1], 1);
}

TEST(Stdio, SharedBrokenPipeRestoresBothChannelsAndSignalHandler) {
  int descriptors[2];
  ASSERT_EQ(::pipe(descriptors), 0);
  std::vector<faio::expected<void>> results;
  int before{};
  int after{};
  int error_after{};
  struct sigaction original{};
  struct sigaction current{};
  ASSERT_EQ(::sigaction(SIGPIPE, nullptr, &original), 0);
  {
    std::fflush(stdout);
    std::fflush(stderr);
    descriptor_restore restore_output{STDOUT_FILENO};
    descriptor_restore restore_error{STDERR_FILENO};
    ASSERT_GE(::dup2(descriptors[1], STDOUT_FILENO), 0);
    ASSERT_GE(::dup2(descriptors[1], STDERR_FILENO), 0);
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    before = ::fcntl(STDOUT_FILENO, F_GETNOSIGPIPE);
    ASSERT_GE(before, 0);
    std::vector<faio::task<faio::expected<void>>> pending;
    for (int index = 0; index != 32; ++index) {
      pending.push_back(faio::stdio::out().write_all("out"));
      pending.push_back(faio::stdio::err().write_all("err"));
    }
    faio::runtime::detail::runtime_context runtime{faio_test::config()};
    results = runtime.block_on(faio::join_all(std::move(pending)));
    after = ::fcntl(STDOUT_FILENO, F_GETNOSIGPIPE);
    error_after = ::fcntl(STDERR_FILENO, F_GETNOSIGPIPE);
  }
  ASSERT_EQ(::sigaction(SIGPIPE, nullptr, &current), 0);
  EXPECT_EQ(original.sa_handler, current.sa_handler);
  EXPECT_EQ(before, after);
  EXPECT_EQ(error_after, after);
  ASSERT_EQ(results.size(), 64);
  for (const auto& result : results) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().value(), EPIPE);
  }
}
#endif
#endif
