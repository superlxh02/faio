/**
 * @file test_process.cpp
 * @brief 进程命令、退出状态与输出采集的单元测试。
 */
#include "test_helpers.hpp"
#include <faio/process/command.hpp>
#include <gtest/gtest.h>
#ifdef FAIO_BAZEL_RUNFILES
#include "bazel_runfiles.hpp"
#endif

namespace {
using namespace faio::process;

/** @brief 创建指向测试子程序的命令。 */
command child(std::string mode, std::string value) {
#ifdef FAIO_BAZEL_RUNFILES
#ifdef _WIN32
  const auto executable = faio_test::bazel_runfile("tests/process_child.exe");
#else
  const auto executable = faio_test::bazel_runfile("tests/process_child");
#endif
#else
  const auto executable = FAIO_PROCESS_TEST_CHILD;
#endif
  return command{executable}.args({std::move(mode), std::move(value)});
}

/** @brief 等待子进程并检查退出码。 */
faio::task<bool> check_exit_status() {
  const auto success = faio_test::take(co_await child("exit", "0").status());
  const auto failure = faio_test::take(co_await child("exit", "7").status());
  co_return success.success() && failure.code() == 7u && !failure.success();
}

/** @brief 同时采集标准输出与错误输出并验证容量限制。 */
faio::task<bool> capture_output() {
  capture_options options;
  options.operation.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  const auto result = faio_test::take(co_await child("write-both", "4096").output(options));
  if (!result.status.success() || result.stdout_data.size() != 4096
      || result.stderr_data.size() != 4096)
    co_return false;
  options.max_stdout_bytes = 4;
  options.max_stderr_bytes = 4;
  options.max_total_bytes = 8;
  const auto limited = co_await child("write-both", "16").output(options);
  co_return !limited && limited.error().value() == static_cast<int>(errc::OUTPUT_LIMIT_EXCEEDED);
}
}  // namespace

/** @brief 命令值保存参数、环境与标准流选项。 */
TEST(Process, CommandConfiguration) {
  command command{"program"};
  command.args({"one", "二"}).env("KEY", "value").set_stdout(stdio::piped());
  EXPECT_EQ(command.get_args().size(), 2u);
  EXPECT_EQ(command.get_envs().size(), 1u);
  EXPECT_EQ(command.get_stdout().kind(), stdio_kind::PIPED);
  command.args_clear();
  EXPECT_TRUE(command.get_args().empty());
}

/** @brief 进程状态保留成功与非零退出结果。 */
TEST(Process, exit_status) {
  faio::runtime::detail::runtime_context context{faio_test::config()};
  EXPECT_TRUE(context.block_on(check_exit_status()));
}

/** @brief 输出采集同时排空两个通道并遵守容量上限。 */
TEST(Process, CapturedOutput) {
  faio::runtime::detail::runtime_context context{faio_test::config()};
  EXPECT_TRUE(context.block_on(capture_output()));
}
