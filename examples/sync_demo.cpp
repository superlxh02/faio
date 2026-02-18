#include "faio/faio.hpp"
#include "fastlog/fastlog.hpp"

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
  fastlog::console.info("  task {} done (counter={})", id, g_counter);
  co_return;
}

void example_mutex(faio::runtime_context &ctx) {
  fastlog::console.info("===== 示例1: mutex =====");
  g_counter = 0;

  faio::block_on<void>(ctx, []() -> faio::task<void> {
    faio::spawn(increment_with_mutex(1, 100));
    faio::spawn(increment_with_mutex(2, 100));
    faio::spawn(increment_with_mutex(3, 100));
    co_return;
  }());

  fastlog::console.info("  final counter = {} (expected 300)", g_counter);
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
  fastlog::console.info("  waiter {} woke up", id);
  g_cv_mutex.unlock();
  co_return;
}

faio::task<void> signal_after_delay() {
  co_await faio::time::sleep(std::chrono::milliseconds(50));
  co_await g_cv_mutex.lock();
  g_ready = true;
  g_cv_mutex.unlock();
  g_cv.notify_all();
  fastlog::console.info("  signaller: notified all");
  co_return;
}

void example_condition_variable(faio::runtime_context &ctx) {
  fastlog::console.info("===== 示例2: ConditionVariable =====");
  g_ready = false;

  faio::block_on<void>(ctx, []() -> faio::task<void> {
    faio::spawn(wait_for_ready(1));
    faio::spawn(wait_for_ready(2));
    faio::spawn(signal_after_delay());
    co_return;
  }());
}

// ============================================================================
// 示例3: Channel 通道
// Channel<T>::make(cap) 得到 Sender 和 Receiver，send/recv 返回
// task<expected<...>>。
// ============================================================================

faio::task<void> channel_sender(faio::sync::channel<int>::Sender sender,
                                int count) {
  for (int i = 0; i < count; ++i) {
    auto result = co_await sender.send(i);
    if (!result) {
      fastlog::console.info("  sender: channel closed");
      co_return;
    }
  }
  fastlog::console.info("  sender: sent {} values", count);
  co_return;
}

faio::task<void> channel_receiver(faio::sync::channel<int>::Receiver receiver,
                                  int expect_count) {
  int received = 0;
  while (received < expect_count) {
    auto result = co_await receiver.recv();
    if (!result) {
      fastlog::console.info("  receiver: channel closed after {}", received);
      co_return;
    }
    fastlog::console.info("  receiver: got {}", *result);
    ++received;
  }
  fastlog::console.info("  receiver: done, total {}", received);
  co_return;
}

void example_channel(faio::runtime_context &ctx) {
  fastlog::console.info("===== 示例3: Channel =====");

  auto [sender, receiver] = faio::sync::channel<int>::make(2);

  faio::block_on<void>(ctx,
                       [sender, receiver]() mutable -> faio::task<void> {
                         faio::spawn(channel_sender(sender, 5));
                         faio::spawn(channel_receiver(receiver, 5));
                         co_return;
                       }());
}

int main() {
  fastlog::set_consolelog_level(fastlog::LogLevel::Info);
  faio::runtime_context ctx;

  example_mutex(ctx);
  example_condition_variable(ctx);
  example_channel(ctx);

  fastlog::console.info("===== all sync examples done =====");
  return 0;
}
