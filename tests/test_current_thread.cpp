#include "backend_test_support.hpp"
#include "faio/faio.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <future>
#include <mutex>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

namespace {

faio::task<int> run_on_caller(std::thread::id caller) {
  if (std::this_thread::get_id() != caller)
    throw std::logic_error("协程未运行在 block_on 调用线程");
  co_await faio::time::sleep(1ms);
  if (std::this_thread::get_id() != caller)
    throw std::logic_error("定时器后协程迁移了线程");
  auto result = faio::spawn_blocking([] { return 42; });
  co_return co_await result;
}

faio::task<void> mark(std::atomic<bool>& done) {
  done.store(true, std::memory_order_release);
  co_return;
}

faio::task<void> empty() { co_return; }

faio::task<void> wait_for_external(std::atomic<bool>& done) {
  for (int attempt = 0; attempt < 100 && !done.load(std::memory_order_acquire);
       ++attempt)
    co_await faio::time::sleep(1ms);
  if (!done.load(std::memory_order_acquire))
    throw std::logic_error("跨线程提交未被驱动");
}

} // namespace

TEST(CurrentThreadRuntimeTest, DrivesOnCallingThreadAndAcceptsExternalSpawn) {
  faio::runtime::configure(faio_test::config_builder()
                               .set_mode(faio::runtime::mode::current_thread)
                               .set_max_blocking_threads(2)
                               .build());
  const auto caller = std::this_thread::get_id();
  EXPECT_EQ(faio::block_on(run_on_caller(caller)), 42);

  std::atomic<bool> done{false};
  std::thread submitter([&] { faio::spawn_detached(mark(done)); });
  submitter.join();
  EXPECT_FALSE(done.load(std::memory_order_acquire));
  faio::block_on(empty());
  EXPECT_TRUE(done.load(std::memory_order_acquire));

  done.store(false, std::memory_order_release);
  std::thread during_drive([&] {
    std::this_thread::sleep_for(2ms);
    faio::spawn_detached(mark(done));
  });
  faio::block_on(wait_for_external(done));
  during_drive.join();
  EXPECT_TRUE(done.load(std::memory_order_acquire));
  done.store(false, std::memory_order_release);
  faio::spawn_detached(mark(done));
  faio::runtime::shutdown();
  EXPECT_TRUE(done.load(std::memory_order_acquire));
}

namespace {
using context = faio::runtime::detail::runtime_context;
auto single_config() {
  return faio_test::config_builder().set_mode(faio::runtime::mode::current_thread)
      .set_max_blocking_threads(4).build();
}
faio::task<int> await_value(faio::join_handle<int>& handle) { co_return co_await handle; }
faio::task<int> delayed_value(faio::runtime::detail::io_engine*& engine) {
  engine = faio::runtime::detail::current_io_engine;
  co_await faio::time::sleep(5ms);
  if (engine != faio::runtime::detail::current_io_engine)
    throw std::logic_error("I/O engine changed between calls");
  co_return 42;
}
faio::task<int> receive_byte(int fd, bool& armed) {
  char byte;
  armed = true;
  const auto result = co_await faio::io::detail::Recv{fd, &byte, 1, 0};
  co_return result ? static_cast<int>(result.value()) : -result.error().value();
}
faio::task<void> unawaited_blocking(std::atomic<bool>& finished) {
  auto handle = faio::spawn_blocking([&] {
    std::this_thread::sleep_for(2ms);
    finished.store(true, std::memory_order_release);
    return 42;
  });
  co_return; // Group completion still includes this blocking callable.
}
faio::task<int> wait_gate(faio::sync::semaphore& gate) {
  co_await gate.acquire();
  co_return 42;
}
faio::task<int> record(std::vector<int>& order, int value) {
  order.push_back(value);
  co_return value;
}
faio::task<void> grow_local_fifo(std::vector<int>& order) {
  std::vector<faio::join_handle<int>> handles;
  for (int i = 0; i < 1024; ++i) handles.push_back(faio::spawn(record(order, i)));
  for (int i = 0; i < 1024; ++i)
    if (co_await handles[i] != i) throw std::logic_error("FIFO result");
}
faio::task<int> yielding_value() {
  for (int i = 0; i < 100; ++i) co_await faio::this_coro::yield();
  co_return 42;
}
faio::task<void> busy_until_timer(bool& done, std::size_t& turns) {
  while (!done && turns < 1000000) {
    ++turns;
    co_await faio::this_coro::yield();
  }
}
faio::task<void> timer_under_load(std::size_t& turns) {
  bool done = false;
  auto handle = faio::spawn(busy_until_timer(done, turns));
  co_await faio::time::sleep(1ms);
  done = true;
  co_await handle;
}
faio::task<void> throwing_task() { throw std::runtime_error("expected"); co_return; }
faio::task<void> reenter(context& ctx) {
  EXPECT_THROW(ctx.block_on(empty()), std::logic_error);
  co_return;
}
faio::task<void> socket_echo(int fd, std::size_t n) {
  char byte;
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ((co_await faio::io::detail::Recv{fd, &byte, 1, 0}).value(), 1u);
    EXPECT_EQ((co_await faio::io::detail::Send{fd, &byte, 1, MSG_NOSIGNAL}).value(), 1u);
  }
}
faio::task<void> socket_ping(int fd, int peer_fd) {
  auto peer = faio::spawn(socket_echo(peer_fd, 100));
  char byte = 'x';
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ((co_await faio::io::detail::Send{fd, &byte, 1, MSG_NOSIGNAL}).value(), 1u);
    EXPECT_EQ((co_await faio::io::detail::Recv{fd, &byte, 1, 0}).value(), 1u);
  }
  co_await peer;
}
struct sockets {
  int fds[2]{-1, -1};
  sockets() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
      throw std::runtime_error("socketpair");
  }
  ~sockets() { ::close(fds[0]); ::close(fds[1]); }
};
}

TEST(CurrentThreadRuntimeTest, DefaultModeIsMultiThreadWithoutSetter) {
  EXPECT_EQ(faio_test::config_builder().build()._mode, faio::runtime::mode::multi_thread);
}

TEST(CurrentThreadRuntimeTest, BackgroundTimerSurvivesBlockOnAndTlsIsRestored) {
  context ctx{single_config()};
  faio::runtime::detail::io_engine* engine = nullptr;
  auto handle = ctx.spawn_observed(delayed_value(engine));
  ctx.block_on(empty());
  ASSERT_NE(engine, nullptr);
  EXPECT_FALSE(handle.done());
  EXPECT_EQ(faio::runtime::detail::current_io_engine, nullptr);
  EXPECT_EQ(faio::runtime::detail::timer::current_timer, nullptr);
  EXPECT_EQ(ctx.block_on(await_value(handle)), 42);
  EXPECT_EQ(faio::detail::current_execution_thread, nullptr);
}

TEST(CurrentThreadRuntimeTest, PendingIoSurvivesBetweenBlockOnCalls) {
  sockets pair;
  context ctx{single_config()};
  bool armed = false;
  auto handle = ctx.spawn_observed(receive_byte(pair.fds[0], armed));
  ctx.block_on(empty());
  EXPECT_TRUE(armed);
  EXPECT_FALSE(handle.done());
  const char byte = 'x';
  ASSERT_EQ(::send(pair.fds[1], &byte, 1, MSG_NOSIGNAL), 1);
  EXPECT_EQ(ctx.block_on(await_value(handle)), 1);
}

TEST(CurrentThreadRuntimeTest, ReadyTasksDoNotStarveTimers) {
  context ctx{single_config()};
  std::size_t turns = 0;
  ctx.block_on(timer_under_load(turns));
  EXPECT_LT(turns, 1000000u);
}

TEST(CurrentThreadRuntimeTest, SubmissionPressureFlushesInsteadOfLosingIo) {
  sockets pair;
  auto config = single_config();
  config._num_events = 1;
  config._submit_interval = 4;
  context ctx{config};
  ctx.block_on(socket_ping(pair.fds[0], pair.fds[1]));
}

TEST(CurrentThreadRuntimeTest, ExceptionsAndRecursiveEntryLeaveRuntimeUsable) {
  context ctx{single_config()};
  EXPECT_THROW(ctx.block_on(throwing_task()), std::runtime_error);
  EXPECT_EQ(faio::runtime::detail::current_io_engine, nullptr);
  ctx.block_on(reenter(ctx));
  EXPECT_EQ(ctx.block_on(yielding_value()), 42);
}

TEST(CurrentThreadRuntimeTest, PendingIoCanBeCancelledWhileDriverIsPaused) {
  sockets pair;
  context ctx{single_config()};
  bool armed = false;
  auto handle = ctx.spawn_observed(receive_byte(pair.fds[0], armed));
  ctx.block_on(empty());
  ASSERT_TRUE(armed);
  handle.request_stop();
  EXPECT_EQ(ctx.block_on(await_value(handle)), -ECANCELED);
}

TEST(CurrentThreadRuntimeTest, UnrelatedBackgroundRootDoesNotHoldBlockOnOpen) {
  context ctx{single_config()};
  faio::sync::semaphore gate{0};
  auto handle = ctx.spawn_observed(wait_gate(gate));
  ctx.block_on(empty());
  EXPECT_FALSE(handle.done());
  gate.release();
  EXPECT_EQ(ctx.block_on(await_value(handle)), 42);
}

TEST(CurrentThreadRuntimeTest, CompletionWithoutCoroutineWakeReleasesDriver) {
  context ctx{single_config()};
  std::atomic<bool> finished{false};
  for (int i = 0; i < 40; ++i) {
    finished.store(false, std::memory_order_relaxed);
    ctx.block_on(unawaited_blocking(finished));
    EXPECT_TRUE(finished.load(std::memory_order_acquire));
  }
}

TEST(CurrentThreadRuntimeTest, StopDrainsBlockingWorkWithoutAnyAsyncRoot) {
  context ctx{single_config()};
  auto handle = ctx.submit_blocking([] { std::this_thread::sleep_for(2ms); return 42; });
  ctx.stop();
  EXPECT_EQ(handle.get(), 42);
  EXPECT_THROW(ctx.block_on(empty()), std::logic_error);
}

TEST(CurrentThreadRuntimeTest, LocalFifoGrowsWithoutDroppingOrReorderingTasks) {
  context ctx{single_config()};
  std::vector<int> order;
  ctx.block_on(grow_local_fifo(order));
  ASSERT_EQ(order.size(), 1024u);
  for (int i = 0; i < 1024; ++i) EXPECT_EQ(order[i], i);
}

TEST(CurrentThreadRuntimeTest, ConcurrentDriversSerializeAndRestoreTls) {
  context ctx{single_config()};
  std::atomic<int> completed{0};
  auto drive = [&] {
    for (int i = 0; i < 30; ++i) {
      if (ctx.block_on(yielding_value()) == 42) ++completed;
      EXPECT_EQ(faio::runtime::detail::current_io_engine, nullptr);
    }
  };
  std::thread first(drive), second(drive);
  first.join(); second.join();
  EXPECT_EQ(completed.load(), 60);
}

TEST(CurrentThreadRuntimeTest, ExternalProducersDuringDriveDoNotLoseTasks) {
  context ctx{single_config()};
  std::atomic<int> finished{0};
  const auto increment = [](std::atomic<int>& count) -> faio::task<void> {
    count.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };
  std::vector<std::thread> producers;
  for (int p = 0; p < 4; ++p)
    producers.emplace_back([&] { for (int i = 0; i < 1000; ++i) ctx.submit(increment(finished)); });
  ctx.block_on(yielding_value());
  for (auto& producer : producers) producer.join();
  ctx.stop();
  EXPECT_EQ(finished.load(), 4000);
}

TEST(BlockingPoolTest, BurstReservesIdleWorkersAndReusesRetiredCapacity) {
  faio::runtime::detail::blocking_pool pool{4, 5ms};
  // Establish an idle worker before the burst, then verify all four are used.
  std::promise<void> warmed;
  pool.submit([&] { warmed.set_value(); });
  warmed.get_future().wait();
  for (int round = 0; round < 2; ++round) {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    int started = 0;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 4; ++i) {
      auto completion = std::make_shared<std::promise<void>>();
      futures.push_back(completion->get_future());
      pool.submit([&, completion] {
        std::unique_lock lock(mutex);
        ++started;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
        completion->set_value();
      });
    }
    {
      std::unique_lock lock(mutex);
      EXPECT_TRUE(cv.wait_for(lock, 2s, [&] { return started == 4; }));
      released = true;
    }
    cv.notify_all();
    for (auto& future : futures) future.get();
    std::this_thread::sleep_for(20ms); // Exercise timeout retirement and reaping.
  }
  pool.close();
  EXPECT_THROW(pool.submit([] {}), std::logic_error);
}


namespace {
/** @brief 重复短子任务必须共享调用链预算；异常和嵌套也不得重置剩余名额。 */
enum class budget_child_path { ordinary, exception, nested };

faio::task<void> spend_child_budget(bool fail) {
  co_await faio::this_coro::yield_if_needed();
  if (fail)
    throw std::runtime_error("已消费预算的子任务异常");
}

faio::task<void> spend_nested_child_budget() {
  co_await spend_child_budget(false);
}

faio::task<void> observe_budget_peer(const std::size_t &turns,
                                    std::size_t &observed) {
  observed = turns;
  co_return;
}

/** @brief 全部有界，仅读取 FIFO 同伴能否在 256 个同步短任务结束前取得执行权。
 * @details 没有时钟或 sleep 判定；未修复时同伴直到 burst 全部完成才执行。
 */
faio::task<std::size_t> child_budget_fairness(budget_child_path path) {
  std::size_t turns{};
  std::size_t observed = 256;
  auto peer = faio::spawn(observe_budget_peer(turns, observed));
  for (; turns < 256; ++turns) {
    if (path == budget_child_path::nested) {
      co_await spend_nested_child_budget();
    } else {
      try {
        co_await spend_child_budget(path == budget_child_path::exception);
      } catch (const std::runtime_error &) {
        // 异常路径继续 burst，已经消费的预算不能因 take_result 重抛而丢失。
      }
    }
  }
  // 先保存同伴是否已经运行，再排空 peer，确保被引用的局部变量生命周期完整。
  const auto observed_before_join = observed;
  co_await peer;
  co_return observed_before_join;
}
} // namespace

TEST(CurrentThreadRuntimeTest, OrdinaryChildTasksPreserveCooperativeFairness) {
  context ctx{single_config()};
  const auto observed = ctx.block_on(child_budget_fairness(budget_child_path::ordinary));
  EXPECT_GT(observed, 0u);
  EXPECT_LT(observed, 256u);
}

TEST(CurrentThreadRuntimeTest, ExceptionalChildTasksPreserveCooperativeFairness) {
  context ctx{single_config()};
  const auto observed = ctx.block_on(child_budget_fairness(budget_child_path::exception));
  EXPECT_GT(observed, 0u);
  EXPECT_LT(observed, 256u);
}

TEST(CurrentThreadRuntimeTest, NestedChildTasksPreserveCooperativeFairness) {
  context ctx{single_config()};
  const auto observed = ctx.block_on(child_budget_fairness(budget_child_path::nested));
  EXPECT_GT(observed, 0u);
  EXPECT_LT(observed, 256u);
}


namespace {
/** @brief 转发到真实 runtime 的队列，仅计数显式/条件让出，测试不依赖队列先后或时钟。 */
struct budget_forwarding_scheduler {
  faio::scheduler_ref target;
  std::atomic<unsigned> enqueued{0};
  void enqueue(std::coroutine_handle<> handle) {
    enqueued.fetch_add(1, std::memory_order_relaxed);
    target.schedule(handle);
  }
  /** @brief 透明转发让出入口，保持真实 runtime 的 FIFO 选择与新执行链边界。 */
  void enqueue_yield(std::coroutine_handle<> handle) {
    enqueued.fetch_add(1, std::memory_order_relaxed);
    target.schedule_yield(handle); // 不能把公平让出降成普通 fast 入队。
  }
};

/** @brief 旧 poll 剩余一份额度；实际重调度后不应因旧额度再执行一次条件让出。 */
faio::task<bool> budget_refresh_after_real_scheduler_resume() {
  for (unsigned attempt = 0; attempt < 63; ++attempt)
    co_await spend_child_budget(false);
  co_await faio::this_coro::yield(); // 真正进入 runtime 队列，必须开启新的 poll 预算。
  co_await spend_child_budget(false);
  co_return true;
}
} // namespace

TEST(CurrentThreadRuntimeTest, ActualSchedulerResumeRefreshesSharedBudgetInBothModes) {
  for (const auto mode : {faio::runtime::mode::current_thread,
                          faio::runtime::mode::multi_thread}) {
    context ctx{faio_test::config_builder().set_mode(mode).set_num_workers(1).build()};
    budget_forwarding_scheduler forwarder{ctx.scheduler()};
    auto operation = budget_refresh_after_real_scheduler_resume();
    auto handle = operation.take();
    // 使用公开 task.take 转交稳定 promise；调度器仅转发，实际恢复仍由本模式 runtime 完成。
    handle.promise().context.scheduler = faio::scheduler_ref{forwarder};
    EXPECT_TRUE(ctx.block_on(faio::task<bool>{handle}));
    EXPECT_EQ(forwarder.enqueued.load(std::memory_order_relaxed), 1u)
        << "实际 resume 后不能因上一 poll 的剩余预算再条件让出";
  }
}
