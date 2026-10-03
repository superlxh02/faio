#include "backend_test_support.hpp"
#include <gtest/gtest.h>

#include "faio/faio.hpp"

// 具名谓词具有外部链接，避免 GCC 在模板协程帧里存放局部 lambda 时发出
// -Wsubobject-linkage；实际 API 仍接受任意可调用谓词。
struct sync_ready_predicate {
  bool &ready;
  bool operator()() const noexcept { return ready; }
};

namespace {

auto mutex_worker(faio::sync::mutex &mtx, int &shared, int loops)
    -> faio::task<int> {
  for (int i = 0; i < loops; ++i) {
    co_await mtx.lock();
    shared += 1;
    mtx.unlock();
    co_await faio::time::sleep(std::chrono::milliseconds(1));
  }
  co_return loops;
}

auto condition_waiter(faio::sync::condition_variable &cv,
                      faio::sync::mutex &mtx, bool &ready, int &observed)
    -> faio::task<void> {
  co_await mtx.lock();
  co_await cv.wait(mtx, sync_ready_predicate{ready});
  observed = 1;
  mtx.unlock();
  co_return;
}

auto condition_notifier(faio::sync::condition_variable &cv,
                        faio::sync::mutex &mtx, bool &ready)
    -> faio::task<void> {
  co_await faio::time::sleep(std::chrono::milliseconds(5));
  co_await mtx.lock();
  ready = true;
  mtx.unlock();
  cv.notify_one();
  co_return;
}

auto condition_run() -> faio::task<int> {
  faio::sync::condition_variable cv;
  faio::sync::mutex mtx;
  bool ready = false;
  int observed = 0;

  faio::spawn_detached(condition_waiter(cv, mtx, ready, observed));
  faio::spawn_detached(condition_notifier(cv, mtx, ready));
  co_await faio::time::sleep(std::chrono::milliseconds(20));
  co_return observed;
}

auto mpsc_run() -> faio::task<int> {
  // GCC 15 的协程结构化绑定析构缺陷：端点放在具名 pair 中。
  auto endpoints = faio::sync::mpsc<int>::make(8);
  auto &sender = endpoints.first;
  auto &receiver = endpoints.second;
  auto send_res = co_await sender.send(52);
  if (!send_res) {
    co_return -1;
  }

  auto recv_res = co_await receiver.recv();
  if (!recv_res) {
    co_return -1;
  }
  co_return recv_res.value();
}

auto raii_run() -> faio::task<int> {
  faio::sync::mutex m;
  faio::sync::semaphore s{1};
  {
    auto lock = co_await m.scoped_lock();
    auto permit = co_await s.acquire_permit();
    if (m.try_lock() || s.available_permits() != 0)
      co_return -1;
  }
  if (!m.try_lock())
    co_return -2;
  m.unlock();
  co_return s.try_acquire() ? 1 : -3;
}

} // namespace

TEST(SyncTest, MutexProtectsSharedState) {
  faio::runtime::detail::runtime_context ctx{
      faio_test::config_builder().build()};
  faio::sync::mutex mtx;
  int shared = 0;
  auto [a, b] = ctx.wait_all(mutex_worker(mtx, shared, 32),
                             mutex_worker(mtx, shared, 32));
  EXPECT_EQ(a + b, 64);
  EXPECT_EQ(shared, 64);
}

TEST(SyncTest, ConditionVariableWakesWaiter) {
  faio::runtime::detail::runtime_context ctx{
      faio_test::config_builder().build()};
  const int observed = ctx.block_on(condition_run());
  EXPECT_EQ(observed, 1);
}

TEST(SyncTest, MpscSendRecvWorks) {
  faio::runtime::detail::runtime_context ctx{
      faio_test::config_builder().build()};
  const int value = ctx.block_on(mpsc_run());
  EXPECT_EQ(value, 52);
}

TEST(SyncTest, RaiiAwaitersReleaseWithoutExtraTaskFrame) {
  faio::runtime::detail::runtime_context ctx{
      faio_test::config_builder().build()};
  EXPECT_EQ(ctx.block_on(raii_run()), 1);
}
