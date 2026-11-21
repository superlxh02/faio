/**
 * @file experimental/test_lifecycle.cpp
 * @brief 协程生命周期注解的单元测试。
 */
#include <faio/faio.hpp>
#include <faio/experimental/lifecycle.h>
#include <gtest/gtest.h>

namespace {
/** @brief 记录协程开始与结束。 */
struct policy {
  inline static int started{};  /**< 已开始的协程数。 */
  inline static int finished{}; /**< 已结束的协程数。 */

  /** @brief 记录进入协程。 */
  void before() { ++started; }

  /** @brief 记录离开协程。 */
  void after(const faio::task_exit&) noexcept { ++finished; }
};

/** @brief 返回带生命周期策略的结果。 */
[[= faio::experimental::lifecycle<policy>{}]] faio::task<int> annotated_value() {
  co_return 42;
}
}  // namespace

/** @brief 注解策略在协程执行前后各调用一次。 */
TEST(ExperimentalLifecycle, Hooks) {
  policy::started = policy::finished = 0;
  faio::runtime::detail::runtime_context runtime{
      faio::ConfigBuilder{}.set_mode(faio::runtime::mode::current_thread).build()};
  EXPECT_EQ(runtime.block_on(annotated_value()), 42);
  EXPECT_EQ(policy::started, 1);
  EXPECT_EQ(policy::finished, 1);
}
