/**
 * @file test_sync.cpp
 * @brief 协程互斥、通知、许可与消息通道的单元测试。
 */
#include "test_helpers.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <vector>

namespace {
/** @brief 在互斥保护下累加共享值。 */
faio::task<int> increment_locked(faio::sync::mutex& mutex, int& value) {
  for (int iteration = 0; iteration != 16; ++iteration) {
    auto lock = co_await mutex.scoped_lock();
    ++value;
    co_await faio::this_coro::yield();
  }
  co_return 16;
}

/** @brief 持锁检查共享状态，尚未就绪时等待通知。 */
faio::task<bool> wait_ready(faio::sync::condition_variable& condition,
                            faio::sync::mutex& mutex,
                            bool& ready) {
  co_await mutex.lock();
  while (!ready)
    co_await condition.wait(mutex, [&] { return ready; });
  mutex.unlock();
  co_return true;
}

/** @brief 更新共享状态并通知等待者。 */
faio::task<bool> notify_ready(faio::sync::condition_variable& condition,
                              faio::sync::mutex& mutex,
                              bool& ready) {
  co_await mutex.lock();
  ready = true;
  mutex.unlock();
  condition.notify_one();
  co_return true;
}

/** @brief 发送有序消息并关闭通道。 */
template <class sender_type>
faio::task<void> send_sequence(sender_type sender) {
  for (int value = 0; value != 16; ++value)
    faio_test::take(co_await sender.send(value));
  sender.close();
}

/** @brief 从有界通道读取消息并验证顺序。 */
faio::task<bool> channel_roundtrip(std::size_t capacity) {
  auto endpoints = faio::sync::mpsc<int>::make(capacity);
  auto sender = faio::spawn(send_sequence(std::move(endpoints.first)));
  for (int value = 0; value != 16; ++value)
    if (faio_test::take(co_await endpoints.second.recv()) != value)
      co_return false;
  const auto closed = co_await endpoints.second.recv();
  co_await sender;
  co_return !closed;
}

/** @brief 检查 RAII 锁与许可是否在作用域结束时释放。 */
faio::task<bool> release_guards() {
  faio::sync::mutex mutex;
  faio::sync::semaphore semaphore{1};
  {
    auto lock = co_await mutex.scoped_lock();
    auto permit = co_await semaphore.acquire_permit();
    if (mutex.try_lock() || semaphore.available_permits() != 0)
      co_return false;
  }
  const bool available = mutex.try_lock();
  if (available)
    mutex.unlock();
  co_return available&& semaphore.try_acquire();
}

/** @brief 提交子任务，在倒计数归零后检查所有写入。 */
faio::task<bool> latch_roundtrip() {
  faio::sync::latch latch{8};
  std::array<int, 8> values{};
  std::vector<faio::join_handle<void>> children;
  auto complete = [](faio::sync::latch& gate, int& value) -> faio::task<void> {
    value = 42;
    gate.count_down();
    co_return;
  };
  for (auto& value : values)
    children.push_back(faio::spawn(complete(latch, value)));
  co_await latch.wait();
  const bool visible =
      std::all_of(values.begin(), values.end(), [](int value) { return value == 42; });
  for (auto& child : children)
    co_await child;
  co_return visible;
}
}  // namespace

/** @brief 竞争任务通过互斥锁串行更新共享状态。 */
TEST(Sync, Mutex) {
  faio::runtime::detail::runtime_context context{faio_test::config()};
  faio::sync::mutex mutex;
  int value{};
  context.wait_all(increment_locked(mutex, value), increment_locked(mutex, value));
  EXPECT_EQ(value, 32);
}

/** @brief 条件变量支持通知先到和等待先到。 */
TEST(Sync, ConditionVariable) {
  faio::runtime::detail::runtime_context context{faio_test::config()};
  faio::sync::mutex mutex;
  faio::sync::condition_variable condition;
  bool ready{};
  auto results =
      context.wait_all(wait_ready(condition, mutex, ready), notify_ready(condition, mutex, ready));
  EXPECT_TRUE(std::get<0>(results));
}

/** @brief 容量为二次幂或其他值的通道均保持顺序、背压与关闭语义。 */
TEST(Sync, BoundedChannel) {
  faio::runtime::detail::runtime_context context{faio_test::config()};
  EXPECT_TRUE(context.block_on(channel_roundtrip(3)));
  EXPECT_TRUE(context.block_on(channel_roundtrip(8)));
}

/** @brief 作用域锁与许可自动释放资源。 */
TEST(Sync, ScopedGuards) {
  faio::runtime::detail::runtime_context context{faio_test::config()};
  EXPECT_TRUE(context.block_on(release_guards()));
}

/** @brief 验证倒计数归零后可见子任务的写入，并检查参数边界。 */
TEST(Sync, Latch) {
  faio::runtime::detail::runtime_context context{faio_test::config()};
  EXPECT_TRUE(context.block_on(latch_roundtrip()));
  EXPECT_THROW(faio::sync::latch(-1), std::invalid_argument);
  faio::sync::latch ready{0};
  EXPECT_TRUE(ready.try_wait());
  EXPECT_THROW(ready.count_down(1), std::invalid_argument);
}
