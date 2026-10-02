#include "faio/faio.hpp"
#include "faio/log.hpp"

// ============================================================================
// 示例1: mutex 互斥锁
// co_await mutex.lock() 获取锁，mutex.unlock() 释放锁。
// ============================================================================

static int g_counter = 0;
static faio::sync::mutex g_mutex;

faio::task<void> increment_with_mutex(int id, int times) {
  for (int i = 0; i < times; ++i) {
    co_await g_mutex.lock();
    ++g_counter;
    g_mutex.unlock();
  }
  faio::log::logger()->info("  task {} done (counter={})", id, g_counter);
  co_return;
}

void example_mutex() {
  faio::log::logger()->info("===== 示例1: mutex =====");
  g_counter = 0;

  faio::block_on<void>([]() -> faio::task<void> {
    faio::spawn_detached(increment_with_mutex(1, 100));
    faio::spawn_detached(increment_with_mutex(2, 100));
    faio::spawn_detached(increment_with_mutex(3, 100));
    co_return;
  }());

  faio::log::logger()->info("  final counter = {} (expected 300)", g_counter);
}

// ============================================================================
// 示例2: ConditionVariable 条件变量
// 配合 mutex 使用：先 co_await mutex.lock()，再 co_await cv.wait(mutex,
// predicate)。
// ============================================================================

static faio::sync::mutex g_cv_mutex;
static faio::sync::condition_variable g_cv;
static bool g_ready = false;

faio::task<void> wait_for_ready(int id) {
  co_await g_cv_mutex.lock();
  co_await g_cv.wait(g_cv_mutex, [] { return g_ready; });
  faio::log::logger()->info("  waiter {} woke up", id);
  g_cv_mutex.unlock();
  co_return;
}

faio::task<void> signal_after_delay() {
  co_await faio::time::sleep(std::chrono::milliseconds(50));
  co_await g_cv_mutex.lock();
  g_ready = true;
  g_cv_mutex.unlock();
  g_cv.notify_all();
  faio::log::logger()->info("  signaller: notified all");
  co_return;
}

void example_condition_variable() {
  faio::log::logger()->info("===== 示例2: ConditionVariable =====");
  g_ready = false;

  faio::block_on<void>([]() -> faio::task<void> {
    faio::spawn_detached(wait_for_ready(1));
    faio::spawn_detached(wait_for_ready(2));
    faio::spawn_detached(signal_after_delay());
    co_return;
  }());
}

// ============================================================================
// 示例3: MPSC 队列
// mpsc<T>::make(cap) 得到 sender 和 receiver；send/recv 返回可等待操作，
// co_await 后得到 expected<...>。无竞争路径不创建额外协程帧。
// ============================================================================

faio::task<void> mpsc_sender(faio::sync::mpsc<int>::sender sender,
                             int count) {
  for (int i = 0; i < count; ++i) {
    auto result = co_await sender.send(i);
    if (!result) {
      faio::log::logger()->info("  sender: mpsc closed");
      co_return;
    }
  }
  faio::log::logger()->info("  sender: sent {} values", count);
  co_return;
}

faio::task<void> mpsc_receiver(faio::sync::mpsc<int>::receiver receiver,
                               int expect_count) {
  int received = 0;
  while (received < expect_count) {
    auto result = co_await receiver.recv();
    if (!result) {
      faio::log::logger()->info("  receiver: mpsc closed after {}", received);
      co_return;
    }
    faio::log::logger()->trace("  receiver: got {}", *result);
    ++received;
  }
  faio::log::logger()->info("  receiver: done, total {}", received);
  co_return;
}

void example_mpsc() {
  faio::log::logger()->info("===== 示例3: MPSC =====");

  auto endpoints = faio::sync::mpsc<int>::make(2);

  faio::block_on(faio::join(mpsc_sender(std::move(endpoints.first), 5),
                                 mpsc_receiver(std::move(endpoints.second), 5)));
}

int main() {
  faio::log::logger()->set_level(spdlog::level::info);

  example_mutex();
  example_condition_variable();
  example_mpsc();

  faio::log::logger()->info("===== all sync examples done =====");
  return 0;
}
