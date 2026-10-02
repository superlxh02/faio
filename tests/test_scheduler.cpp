#include "faio/faio.hpp"
#include "faio/detail/runtime/core/scheduler/ready_queue.hpp"
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

namespace scheduler_test {
// 不依赖 runtime 的手工调度器，验证 concept 只要求提交能力。
struct manual_scheduler {
  std::deque<std::coroutine_handle<>> ready; // 尚未恢复的协程，不负责销毁帧。
  bool reject{false};                      // 模拟提交失败，失败前不接管句柄。
  std::size_t submitted{0};                 // 成功提交次数，用于检验协作预算。
  void enqueue(std::coroutine_handle<> handle) {
    if (reject) throw std::runtime_error("拒绝提交");
    ready.push_back(handle);
    ++submitted;
  }
  // 使用与 worker 相同的任务 TLS 边界，线程绑定在整个 pump 期间保持有效。
  void pump() {
    while (!ready.empty()) {
      const auto handle = ready.front();
      ready.pop_front();
      auto* tracker = faio::detail::current_tracker;
      auto stop = faio::detail::current_stop_token;
      faio::detail::current_tracker = nullptr;
      faio::detail::current_stop_token = {};
      handle.resume();
      faio::detail::current_tracker = tracker;
      faio::detail::current_stop_token = std::move(stop);
    }
  }
};
// 单线程手工计数服务，用于验证登记、正常结束及失败回滚。
struct manual_lifetime {
  int active{0};      // 尚未销毁的根帧数。
  int registered{0};  // 累计登记数。
  int finished{0};    // 累计归还数。
  void register_task() noexcept { ++active; ++registered; }
  void finish_task() noexcept { --active; ++finished; }
};
struct invalid_scheduler { void enqueue(int); };
static_assert(faio::coroutine_scheduler<manual_scheduler>);
static_assert(!faio::coroutine_scheduler<invalid_scheduler>);
static_assert(sizeof(faio::scheduler_ref) == 2 * sizeof(void*));
static_assert(!std::is_constructible_v<faio::scheduler_ref, manual_scheduler&&>);

faio::task<int> manual_value(int value) {
  co_await faio::this_coro::yield();
  co_return value;
}
faio::task<int> manual_join() {
  auto [first, second] = co_await faio::join(manual_value(20), manual_value(22));
  co_return first + second;
}
faio::task<void> budget_loop() {
  for (std::size_t i = 0; i < 128; ++i) co_await faio::this_coro::yield_if_needed();
}

// 队列测试使用真实的挂起协程帧；只检查句柄身份，不恢复这些测试帧。
class queue_frame {
public:
  struct promise_type {
    std::size_t id; // 帧身份，用于检测重复领取和丢失。
    explicit promise_type(std::size_t value) noexcept : id(value) {}
    queue_frame get_return_object() noexcept {
      return queue_frame{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    std::suspend_always final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept { std::terminate(); }
  };
  using handle_type = std::coroutine_handle<promise_type>;
  explicit queue_frame(handle_type handle) noexcept : handle_(handle) {}
  queue_frame(queue_frame&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  queue_frame(const queue_frame&) = delete;
  ~queue_frame() { if (handle_) handle_.destroy(); }
  std::coroutine_handle<> handle() const noexcept { return handle_; }
  static std::size_t id(std::coroutine_handle<> handle) noexcept {
    return handle_type::from_address(handle.address()).promise().id;
  }
private:
  handle_type handle_; // 测试结束且所有窃取线程退出后才销毁的独占帧。
};
queue_frame make_queue_frame(std::size_t id) { (void)id; co_return; }

faio::task<void> mark_entered_and_wait(faio::sync::semaphore& signal,
                                     std::atomic<bool>& entered, faio::scheduler_ref expected,
                                     std::atomic<bool>& correct_domain) {
  entered.store(true, std::memory_order_release);
  co_await signal.acquire();
  co_await faio::this_coro::yield();
  correct_domain.store(faio::detail::current_scheduler() == expected, std::memory_order_release);
}
faio::task<void> confirm_suspended(std::atomic<bool>& entered) {
  while (!entered.load(std::memory_order_acquire)) co_await faio::this_coro::yield();
  // 与等待者共用单 worker，到此处时等待者已完成 await_suspend。
}
faio::task<void> release_from_worker(faio::sync::semaphore& signal) { signal.release(); co_return; }
faio::task<void> yielding_root(std::atomic<std::size_t>& completed) {
  for (int i = 0; i < 4; ++i) co_await faio::this_coro::yield();
  completed.fetch_add(1, std::memory_order_relaxed);
}
faio::task<void> set_marker(bool& marker) { marker = true; co_return; }
faio::task<void> receive_external_notifications(faio::sync::semaphore& signal,
                                               std::atomic<std::size_t>& completed,
                                               std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    co_await signal.acquire();
    completed.store(i + 1, std::memory_order_release);
    completed.notify_one();
  }
}
faio::task<std::size_t> check_fast_slot_fairness() {
  bool marker = false;
  auto child = faio::spawn(set_marker(marker));
  std::size_t turns = 0;
  while (!marker && turns < 1000) { ++turns; co_await faio::this_coro::yield(); }
  co_await child;
  co_return turns;
}
// 外部线程拥有 pipe 描述符，所有异步操作退出后再同步关闭。
struct pipe_descriptors {
  std::array<int, 2> descriptors{-1, -1}; // 读端和写端，初始化失败时保持无效。
  pipe_descriptors() {
    if (::pipe(descriptors.data()) != 0) throw std::runtime_error("pipe 创建失败");
  }
  ~pipe_descriptors() { for (auto fd : descriptors) if (fd >= 0) ::close(fd); }
};
faio::task<char> read_pipe(int fd) {
  char value = 0;
  auto result = co_await faio::io::read(fd, &value, 1, 0);
  if (!result || *result != 1) throw std::runtime_error("pipe 读取失败");
  co_return value;
}
faio::task<void> write_pipe_after_timer(int fd) {
  co_await faio::time::sleep(std::chrono::milliseconds(1));
  const char value = 'x';
  auto result = co_await faio::io::write(fd, &value, 1, 0);
  if (!result || *result != 1) throw std::runtime_error("pipe 写入失败");
}
faio::task<bool> cancel_pipe_read(int fd, std::atomic<bool>& entered) {
  char value = 0;
  entered.store(true, std::memory_order_release);
  auto result = co_await faio::io::read(fd, &value, 1, 0);
  co_return !result && result.error().value() == ECANCELED;
}
} // namespace scheduler_test

TEST(SchedulerTest, PureCustomSchedulerRunsJoinWithoutRuntime) {
  scheduler_test::manual_scheduler scheduler;
  scheduler_test::manual_lifetime lifetime;
  int local_state = 0;
  std::optional<faio::join_handle<int>> result;
  {
    faio::detail::execution_thread_binding binding{faio::scheduler_ref{scheduler},
        faio::task_lifetime_ref{lifetime}, local_state, 7};
    faio::detail::execution_thread_guard guard{binding};
    result.emplace(faio::spawn(scheduler_test::manual_join()));
    EXPECT_EQ(lifetime.active, 1);
    scheduler.pump();
    EXPECT_EQ(lifetime.active, 0);
    EXPECT_EQ(lifetime.registered, lifetime.finished);
  }
  EXPECT_EQ(result->get(), 42);
  EXPECT_FALSE(faio::detail::current_scheduler());
}

TEST(SchedulerTest, ThreadBindingRestoresAndChecksLocalTypeAndDomain) {
  scheduler_test::manual_scheduler first, second;
  int first_state = 0;
  double second_state = 0;
  faio::detail::execution_thread_binding outer{faio::scheduler_ref{first}, {}, first_state, 3};
  faio::detail::execution_thread_binding inner{faio::scheduler_ref{second}, {}, second_state, 9};
  EXPECT_EQ(faio::detail::current_worker_id(), faio::detail::no_worker_id);
  {
    faio::detail::execution_thread_guard outer_guard{outer};
    EXPECT_EQ(faio::detail::current_worker_id(), 3u);
    EXPECT_EQ(outer.local_state_for<int>(faio::scheduler_ref{first}), &first_state);
    EXPECT_EQ(outer.local_state_for<double>(faio::scheduler_ref{first}), nullptr);
    EXPECT_EQ(outer.local_state_for<int>(faio::scheduler_ref{second}), nullptr);
    {
      faio::detail::execution_thread_guard inner_guard{inner};
      EXPECT_EQ(faio::detail::current_worker_id(), 9u);
    }
    EXPECT_EQ(faio::detail::current_scheduler(), faio::scheduler_ref{first});
  }
  EXPECT_FALSE(faio::detail::on_runtime_worker());
}

TEST(SchedulerTest, SubmissionFailureReturnsAllLifetimeCounts) {
  scheduler_test::manual_scheduler scheduler;
  scheduler.reject = true;
  scheduler_test::manual_lifetime lifetime;
  faio::detail::task_tracker tracker;
  tracker.add();
  auto root = faio::detail::spawn_coro(scheduler_test::budget_loop(), &tracker);
  EXPECT_THROW(faio::detail::start_detached(std::move(root), faio::scheduler_ref{scheduler},
               &tracker, faio::task_lifetime_ref{lifetime}), std::runtime_error);
  tracker.wait();
  EXPECT_EQ(lifetime.active, 0);
  EXPECT_EQ(lifetime.registered, 1);
  EXPECT_EQ(lifetime.finished, 1);
  tracker.add();
  auto rejected = faio::detail::spawn_coro(scheduler_test::budget_loop(), &tracker);
  EXPECT_THROW(faio::detail::start_detached(std::move(rejected), {}, &tracker,
               faio::task_lifetime_ref{lifetime}), std::logic_error);
  tracker.wait();
  EXPECT_EQ(lifetime.registered, 1); // 空调度器不能归还从未登记的计数。
}

TEST(SchedulerTest, CooperativeBudgetActuallyYieldsEvery64Operations) {
  scheduler_test::manual_scheduler scheduler;
  int local_state = 0;
  faio::detail::execution_thread_binding binding{faio::scheduler_ref{scheduler}, {}, local_state, 0};
  faio::detail::execution_thread_guard guard{binding};
  faio::spawn_detached(scheduler_test::budget_loop());
  scheduler.pump();
  EXPECT_EQ(scheduler.submitted, 3u); // 初次提交 + 第 64 次 + 第 128 次让出。
}

TEST(SchedulerQueueTest, ClosedGlobalQueueRollsBackLocalOverflow) {
  faio::runtime::detail::global_ready_queue global;
  faio::runtime::detail::local_ready_queue<8> local;
  std::vector<scheduler_test::queue_frame> frames;
  for (std::size_t i = 0; i < 10; ++i) frames.push_back(scheduler_test::make_queue_frame(i));
  local.push_local(frames[8].handle(), global);
  for (std::size_t i = 0; i < 8; ++i) local.push_back(frames[i].handle(), global);
  global.close();
  EXPECT_THROW(local.push_local(frames[9].handle(), global), std::logic_error);
  const auto fast = local.try_pop_local();
  ASSERT_TRUE(fast);
  EXPECT_EQ(scheduler_test::queue_frame::id(*fast), 8u);
  for (std::size_t i = 0; i < 8; ++i) {
    const auto task = local.try_pop();
    ASSERT_TRUE(task);
    EXPECT_EQ(scheduler_test::queue_frame::id(*task), i);
  }
  EXPECT_TRUE(local.empty_local());
  EXPECT_TRUE(global.empty());
}

TEST(SchedulerQueueTest, ConcurrentStealingConsumesEveryHandleExactlyOnce) {
  constexpr std::size_t count = 20000;
  faio::runtime::detail::global_ready_queue global;
  faio::runtime::detail::local_ready_queue<64> source;
  std::vector<scheduler_test::queue_frame> frames;
  frames.reserve(count);
  for (std::size_t i = 0; i < count; ++i) frames.push_back(scheduler_test::make_queue_frame(i));
  auto seen = std::make_unique<std::atomic<unsigned>[]>(count);
  std::atomic<bool> producer_done{false};
  auto consume = [&](std::coroutine_handle<> handle) {
    seen[scheduler_test::queue_frame::id(handle)].fetch_add(1, std::memory_order_relaxed);
  };
  std::vector<std::jthread> thieves;
  for (int i = 0; i < 3; ++i) thieves.emplace_back([&] {
    faio::runtime::detail::local_ready_queue<64> destination;
    for (;;) {
      if (auto task = destination.try_pop()) { consume(*task); continue; }
      if (auto task = source.steal_into(destination)) { consume(*task); continue; }
      if (auto task = global.try_pop()) { consume(*task); continue; }
      if (producer_done.load(std::memory_order_acquire) && source.empty() && global.empty()) break;
      std::this_thread::yield();
    }
  });
  for (std::size_t i = 0; i < count; ++i) {
    source.push_back(frames[i].handle(), global);
    if (i % 3 == 0) if (auto task = source.try_pop()) consume(*task);
  }
  while (auto task = source.try_pop()) consume(*task);
  producer_done.store(true, std::memory_order_release);
  thieves.clear(); // join 完成后才读取计数并销毁测试帧。
  for (std::size_t i = 0; i < count; ++i)
    ASSERT_EQ(seen[i].load(std::memory_order_relaxed), 1u) << "frame " << i;
}

TEST(SchedulerTest, CrossRuntimeNotificationReturnsToOriginalDomain) {
  faio::runtime_context first{faio::ConfigBuilder{}.set_num_workers(1).build()};
  faio::runtime_context second{faio::ConfigBuilder{}.set_num_workers(1).build()};
  faio::sync::semaphore signal{0};
  std::atomic<bool> entered{false}, correct_domain{false};
  auto waiter = faio::spawn(first, scheduler_test::mark_entered_and_wait(
      signal, entered, first.scheduler(), correct_domain));
  faio::spawn(first, scheduler_test::confirm_suspended(entered)).get();
  faio::block_on(second, scheduler_test::release_from_worker(signal));
  waiter.get();
  EXPECT_TRUE(correct_domain.load(std::memory_order_acquire));
}

TEST(SchedulerTest, MultipleProducersYieldAndDrainBeforeShutdown) {
  faio::runtime_context context{faio::ConfigBuilder{}.set_num_workers(4).build()};
  std::atomic<std::size_t> completed{0};
  std::vector<std::jthread> producers;
  for (int i = 0; i < 4; ++i) producers.emplace_back([&] {
    for (int j = 0; j < 1000; ++j)
      faio::spawn_detached(context, scheduler_test::yielding_root(completed));
  });
  producers.clear();
  context.stop();
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 4000u);
}

TEST(SchedulerTest, FastSlotCannotStarveLocalFifoTasks) {
  faio::runtime_context context{faio::ConfigBuilder{}.set_num_workers(1).build()};
  EXPECT_LE(faio::block_on(context, scheduler_test::check_fast_slot_fairness()), 9u);
}

TEST(SchedulerTest, IoAndTimerCompletionsUseTheSameReadySink) {
  faio::runtime_context context{faio::ConfigBuilder{}.set_num_workers(2).build()};
  scheduler_test::pipe_descriptors pipe;
  auto [value, ignored] = faio::block_on(context, faio::join(
      scheduler_test::read_pipe(pipe.descriptors[0]),
      scheduler_test::write_pipe_after_timer(pipe.descriptors[1])));
  (void)ignored;
  EXPECT_EQ(value, 'x');
}

TEST(SchedulerTest, CancelledIoCompletesBeforeRuntimeShutdown) {
  faio::runtime_context context{faio::ConfigBuilder{}.set_num_workers(1).build()};
  scheduler_test::pipe_descriptors pipe;
  std::atomic<bool> entered{false};
  auto reader = faio::spawn(context, scheduler_test::cancel_pipe_read(pipe.descriptors[0], entered));
  faio::spawn(context, scheduler_test::confirm_suspended(entered)).get();
  reader.request_stop();
  EXPECT_TRUE(reader.get());
  context.stop();
}

TEST(SchedulerTest, ZeroIdleSpinStillHandlesRepeatedExternalNotifications) {
  faio::runtime_context context{faio::config_builder{}.set_num_workers(1).set_idle_spin_count(0).build()};
  faio::sync::semaphore signal{0};
  std::atomic<std::size_t> completed{0};
  constexpr std::size_t count = 1000;
  auto receiver = faio::spawn(context, scheduler_test::receive_external_notifications(signal, completed, count));
  for (std::size_t i = 0; i < count; ++i) {
    signal.release();
    for (auto seen = completed.load(std::memory_order_acquire); seen <= i;
         seen = completed.load(std::memory_order_acquire))
      completed.wait(seen, std::memory_order_acquire);
  }
  receiver.get();
  EXPECT_EQ(completed.load(std::memory_order_acquire), count);
}

TEST(SchedulerTest, SemaphoreConcurrentReleasePreservesEveryPermit) {
  faio::runtime_context context{faio::config_builder{}.set_num_workers(4).build()};
  faio::sync::semaphore signal{0};
  std::atomic<std::size_t> consumed{0};
  std::vector<faio::join_handle<void>> consumers;
  for (int i = 0; i < 8; ++i) consumers.push_back(faio::spawn(context,
      [](faio::sync::semaphore& sem, std::atomic<std::size_t>& count) -> faio::task<void> {
        for (int j = 0; j < 1000; ++j) {
          co_await sem.acquire();
          count.fetch_add(1, std::memory_order_relaxed);
          if (j % 32 == 0) co_await faio::this_coro::yield();
        }
      }(signal, consumed)));
  std::vector<std::jthread> producers;
  for (int i = 0; i < 4; ++i) producers.emplace_back([&] {
    for (int j = 0; j < 2000; ++j) signal.release();
  });
  producers.clear();
  for (auto& handle : consumers) handle.get();
  EXPECT_EQ(consumed.load(), 8000u);
  EXPECT_EQ(signal.available_permits(), 0);
}

TEST(SchedulerTest, SemaphoreLastCancelledWaiterRestoresFastRelease) {
  faio::runtime_context context{faio::config_builder{}.set_num_workers(1).build()};
  faio::sync::semaphore signal{0};
  std::atomic<bool> entered{false};
  auto handle = faio::spawn(context, [](faio::sync::semaphore& sem,
                                      std::atomic<bool>& started) -> faio::task<void> {
    started.store(true, std::memory_order_release);
    co_await sem.acquire();
  }(signal, entered));
  faio::spawn(context, scheduler_test::confirm_suspended(entered)).get();
  handle.request_stop();
  EXPECT_THROW(handle.get(), faio::operation_cancelled);
  EXPECT_EQ(signal.available_permits(), 0);
  signal.release(3);
  EXPECT_EQ(signal.available_permits(), 3);
  EXPECT_TRUE(signal.try_acquire());
  EXPECT_TRUE(signal.try_acquire());
  EXPECT_TRUE(signal.try_acquire());
  EXPECT_FALSE(signal.try_acquire());
}
