/**
 * @file experimental/test_execution.cpp
 * @brief 实验性 sender 与协程互操作的单元测试。
 */
#include <faio/experimental/execution.h>
#include <gtest/gtest.h>
#include <stdexcept>
#include <tuple>

namespace {
/** @brief 返回供 sender 桥接使用的协程结果。 */
faio::task<int> value_task() {
  co_return 42;
}

/** @brief 在 faio 任务中接入并提交 sender，等待转换后的结果。 */
faio::task<int> bridge_in_current_context() {
  const auto result = co_await faio::experimental::spawn_sender(stdexec::then(
      faio::experimental::as_sender(value_task()), [](int value) { return value + 1; }));
  co_return result ? std::get<0>(*result) : 0;
}
}  // namespace

/** @brief 单线程运行时执行 sender，并转换协程结果。 */
TEST(ExperimentalExecution, CurrentThread) {
  faio::experimental::current_thread_runtime runtime;
  const auto sender = runtime.run(stdexec::just(17));
  ASSERT_TRUE(sender);
  EXPECT_EQ(std::get<0>(*sender), 17);
  const auto task = runtime.run(runtime.as_sender(value_task()));
  ASSERT_TRUE(task);
  EXPECT_EQ(std::get<0>(*task), 42);
}

/** @brief 多线程运行时传播 sender 的值与错误。 */
TEST(ExperimentalExecution, MultiThread) {
  auto options = faio::experimental::multi_thread_defaults();
  options.workers = 2;
  faio::experimental::multi_thread_runtime runtime{options};
  const auto result =
      runtime.run(stdexec::then(stdexec::just(17), [](int value) { return value + 25; }));
  ASSERT_TRUE(result);
  EXPECT_EQ(std::get<0>(*result), 42);
  EXPECT_THROW(runtime.run(stdexec::then(stdexec::just(),
                                         []() -> int { throw std::runtime_error("测试异常"); })),
               std::runtime_error);
}

/** @brief 桥接的非成员函数沿用当前 faio 任务的执行上下文。 */
TEST(ExperimentalExecution, BridgeUsesCurrentContext) {
  faio::experimental::current_thread_runtime runtime;
  const auto result = runtime.run(runtime.as_sender(bridge_in_current_context()));
  ASSERT_TRUE(result);
  EXPECT_EQ(std::get<0>(*result), 43);
}

/** @brief 普通线程没有执行上下文时，桥接的非成员函数报告使用错误。 */
TEST(ExperimentalExecution, BridgeRequiresCurrentContext) {
  EXPECT_THROW((void)faio::experimental::as_sender(value_task()), std::logic_error);
  EXPECT_THROW((void)faio::experimental::spawn_sender(stdexec::just(42)), std::logic_error);
}
