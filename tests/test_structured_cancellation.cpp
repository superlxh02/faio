#include "backend_test_support.hpp"
/**
 * @file test_structured_cancellation.cpp
 * @brief scope 失败传播与 condition_variable 取消时重新取得用户锁的契约。
 */
#include "test_support.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <sys/socket.h>

namespace {
using namespace std::chrono_literals;
faio::task<void> fail_child() {
  co_await faio::time::sleep(3ms);
  throw std::runtime_error("scope child failure");
}
struct sleep_body {
  std::atomic<bool> &started;
  std::atomic<bool> &drained;
  faio::task<void> operator()(faio::scope_context &scope) {
    scope.spawn(fail_child());
    started.store(true, std::memory_order_release);
    try {
      co_await faio::time::sleep(10s);
    } catch (const faio::operation_cancelled &) {
      drained.store(true, std::memory_order_release);
      throw;
    }
  }
};
faio::task<void> failed_sleep_scope(std::atomic<bool> &started,
                                    std::atomic<bool> &drained) {
  co_await faio::scope(sleep_body{started, drained});
}
struct read_body {
  faio::io::detail::resource_ptr resource;
  std::atomic<bool> &drained;
  faio::task<void> operator()(faio::scope_context &scope) {
    scope.spawn(fail_child());
    char byte{};
    const auto result = co_await faio::io::recv(resource, &byte, 1, 0);
    drained.store(!result && result.error().value() == ECANCELED,
                  std::memory_order_release);
  }
};
faio::task<void> failed_read_scope(std::atomic<bool> &drained) {
  auto pair = faio_test::take(faio::net::unix::UnixStream::pair());
  co_await faio::scope(read_body{pair.first.resource(), drained});
}

struct false_predicate {
  bool operator()() const noexcept { return false; }
};
/** @brief CV 取消抛出时必须已经重新取得原锁；返回 true 代表契约完整满足。 */
faio::task<bool> condition_waiter(faio::sync::mutex &mutex,
                                  faio::sync::condition_variable &condition,
                                  std::atomic<bool> &holder_active) {
  co_await mutex.lock();
  try {
    co_await condition.wait(mutex, false_predicate{});
  } catch (const faio::operation_cancelled &) {
    if (holder_active.load(std::memory_order_acquire))
      co_return false;
    if (mutex.try_lock()) {
      mutex.unlock();
      co_return false;
    }
    mutex.unlock();
    co_return true;
  }
  mutex.unlock();
  co_return false;
}
faio::task<void> condition_lock_holder(faio::sync::mutex &mutex,
                                       faio::sync::semaphore &acquired,
                                       faio::sync::semaphore &release,
                                       std::atomic<bool> &active) {
  co_await mutex.lock();
  active.store(true, std::memory_order_release);
  acquired.release();
  co_await release.acquire();
  active.store(false, std::memory_order_release);
  mutex.unlock();
}
faio::task<bool> cancel_condition_while_locked() {
  faio::sync::mutex mutex;
  faio::sync::condition_variable condition;
  faio::sync::semaphore acquired{0}, release{0};
  std::atomic<bool> active{false};
  auto waiter = faio::spawn(condition_waiter(mutex, condition, active));
  co_await faio::time::sleep(3ms);
  auto holder =
      faio::spawn(condition_lock_holder(mutex, acquired, release, active));
  co_await acquired.acquire();
  waiter.request_stop();
  co_await faio::time::sleep(3ms);
  const bool completed_before_lock_release = waiter.done();
  release.release();
  co_await holder;
  const bool reacquired = co_await waiter;
  const bool usable = mutex.try_lock();
  if (usable)
    mutex.unlock();
  co_return !completed_before_lock_release && reacquired && usable;
}
} // namespace

TEST(StructuredCancellationContract,
     FailedChildCancelsSleepingScopeBodyAndDrainsIt) {
  faio_test::runtime_context runtime{
      faio_test::config_builder().set_num_workers(4).build()};
  std::atomic<bool> started{false}, drained{false};
  const auto begin = std::chrono::steady_clock::now();
  EXPECT_THROW(runtime.block_on(failed_sleep_scope(started, drained)),
               std::runtime_error);
  EXPECT_TRUE(started.load());
  EXPECT_TRUE(drained.load());
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 1s);
}
TEST(StructuredCancellationContract, FailedChildCancelsPendingReadInScopeBody) {
  faio_test::runtime_context runtime{
      faio_test::config_builder().set_num_workers(4).build()};
  std::atomic<bool> drained{false};
  EXPECT_THROW(runtime.block_on(failed_read_scope(drained)),
               std::runtime_error);
  EXPECT_TRUE(drained.load());
}
TEST(StructuredCancellationContract,
     ConditionCancellationWaitsForUserMutexBeforeThrowing) {
  faio_test::runtime_context runtime{
      faio_test::config_builder().set_num_workers(4).build()};
  EXPECT_TRUE(runtime.block_on(cancel_condition_while_locked()));
}
