#include <gtest/gtest.h>

#include "faio/faio.hpp"

namespace {

auto mutex_worker(faio::sync::mutex& mtx, int& shared, int loops) -> faio::task<int> {
  for (int i = 0; i < loops; ++i) {
    co_await mtx.lock();
    shared += 1;
    mtx.unlock();
    co_await faio::time::sleep(std::chrono::milliseconds(1));
  }
  co_return loops;
}

auto condition_waiter(faio::sync::condition_variable& cv,
                      faio::sync::mutex& mtx,
                      bool& ready,
                      int& observed) -> faio::task<void> {
  co_await mtx.lock();
  co_await cv.wait(mtx, [&]() { return ready; });
  observed = 1;
  mtx.unlock();
  co_return;
}

auto condition_notifier(faio::sync::condition_variable& cv,
                        faio::sync::mutex& mtx,
                        bool& ready) -> faio::task<void> {
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

  faio::spawn(condition_waiter(cv, mtx, ready, observed));
  faio::spawn(condition_notifier(cv, mtx, ready));
  co_await faio::time::sleep(std::chrono::milliseconds(20));
  co_return observed;
}

auto channel_run() -> faio::task<int> {
  auto [sender, receiver] = faio::sync::channel<int>::make(8);
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

}  // namespace

TEST(SyncTest, MutexProtectsSharedState) {
  faio::runtime_context ctx;
  faio::sync::mutex mtx;
  int shared = 0;
  auto [a, b] = faio::wait_all(ctx,
                               mutex_worker(mtx, shared, 32),
                               mutex_worker(mtx, shared, 32));
  EXPECT_EQ(a + b, 64);
  EXPECT_EQ(shared, 64);
}

TEST(SyncTest, ConditionVariableWakesWaiter) {
  faio::runtime_context ctx;
  const int observed = faio::block_on(ctx, condition_run());
  EXPECT_EQ(observed, 1);
}

TEST(SyncTest, ChannelSendRecvWorks) {
  faio::runtime_context ctx;
  const int value = faio::block_on(ctx, channel_run());
  EXPECT_EQ(value, 52);
}
