#include "backend_test_support.hpp"
#include "frame_allocator_test_types.hpp"
#include "faio/faio.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/ready_queue.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/local_scheduler.hpp"
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <optional>
#include <memory>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

namespace scheduler_test {
// 不依赖 runtime 的手工调度器，验证 concept 只要求提交能力。
struct manual_scheduler {
  std::deque<std::coroutine_handle<>> ready;  // 尚未恢复的协程，不负责销毁帧。
  bool reject{false};                         // 模拟提交失败，失败前不接管句柄。
  std::size_t submitted{0};                   // 成功提交次数，用于检验协作预算。

  void enqueue(std::coroutine_handle<> handle) {
    if (reject)
      throw std::runtime_error("拒绝提交");
    ready.push_back(handle);
    ++submitted;
  }

  // 使用与 worker 相同的任务 TLS 边界，线程绑定在整个 pump 期间保持有效。
  void pump() {
    while (!ready.empty()) {
      const auto handle = ready.front();
      ready.pop_front();
      faio::detail::cooperative_poll_scope
          poll_budget;  // 自定义 scheduler 的真实 resume 同样建立预算。
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

  void register_task() noexcept {
    ++active;
    ++registered;
  }

  void finish_task() noexcept {
    --active;
    ++finished;
  }
};

struct invalid_scheduler {
  void enqueue(int);
};

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
  for (std::size_t i = 0; i < 128; ++i)
    co_await faio::this_coro::yield_if_needed();
}

faio::task<void> manual_budget_step() {
  co_await faio::this_coro::yield_if_needed();
}

faio::task<void> manual_budget_after_actual_resume() {
  for (unsigned attempt = 0; attempt < 63; ++attempt)
    co_await manual_budget_step();
  co_await faio::this_coro::yield();
  co_await manual_budget_step();
}

// 队列测试使用真实的挂起协程帧；只检查句柄身份，不恢复这些测试帧。
class queue_frame {
 public:
  struct promise_type {
    std::size_t id;  // 帧身份，用于检测重复领取和丢失。

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

  ~queue_frame() {
    if (handle_)
      handle_.destroy();
  }

  std::coroutine_handle<> handle() const noexcept { return handle_; }

  static std::size_t id(std::coroutine_handle<> handle) noexcept {
    return handle_type::from_address(handle.address()).promise().id;
  }

 private:
  handle_type handle_;  // 测试结束且所有窃取线程退出后才销毁的独占帧。
};

queue_frame make_queue_frame(std::size_t id) {
  (void)id;
  co_return;
}

faio::task<void> mark_entered_and_wait(faio::sync::semaphore& signal,
                                       std::atomic<bool>& entered,
                                       faio::scheduler_ref expected,
                                       std::atomic<bool>& correct_domain) {
  entered.store(true, std::memory_order_release);
  co_await signal.acquire();
  co_await faio::this_coro::yield();
  correct_domain.store(faio::detail::current_scheduler() == expected, std::memory_order_release);
}

faio::task<void> confirm_suspended(std::atomic<bool>& entered) {
  while (!entered.load(std::memory_order_acquire))
    co_await faio::this_coro::yield();
  // 与等待者共用单 worker，到此处时等待者已完成 await_suspend。
}

faio::task<void> release_from_worker(faio::sync::semaphore& signal) {
  signal.release();
  co_return;
}

faio::task<void> yielding_root(std::atomic<std::size_t>& completed) {
  for (int i = 0; i < 4; ++i)
    co_await faio::this_coro::yield();
  completed.fetch_add(1, std::memory_order_relaxed);
}

faio::task<void> set_marker(bool& marker) {
  marker = true;
  co_return;
}

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
  while (!marker && turns < 1000) {
    ++turns;
    co_await faio::this_coro::yield();
  }
  co_await child;
  co_return turns;
}

// 外部线程拥有 pipe 描述符，所有异步操作退出后再同步关闭。
struct pipe_descriptors {
  std::array<int, 2> descriptors{-1, -1};  // 读端和写端，初始化失败时保持无效。

  pipe_descriptors() {
    if (::pipe(descriptors.data()) != 0)
      throw std::runtime_error("pipe 创建失败");
  }

  ~pipe_descriptors() {
    for (auto fd : descriptors)
      if (fd >= 0)
        ::close(fd);
  }
};

faio::task<char> read_pipe(int fd) {
  char value = 0;
  auto result = co_await faio::io::read(fd, &value, 1, 0);
  if (!result || *result != 1)
    throw std::runtime_error("pipe 读取失败");
  co_return value;
}

faio::task<void> write_pipe_after_timer(int fd) {
  co_await faio::time::sleep(std::chrono::milliseconds(1));
  const char value = 'x';
  auto result = co_await faio::io::write(fd, &value, 1, 0);
  if (!result || *result != 1)
    throw std::runtime_error("pipe 写入失败");
}

faio::task<bool> cancel_pipe_read(int fd, std::atomic<bool>& entered) {
  char value = 0;
  entered.store(true, std::memory_order_release);
  auto result = co_await faio::io::read(fd, &value, 1, 0);
  co_return !result && result.error().value() == ECANCELED;
}
}  // namespace scheduler_test

namespace scheduler_sleep_batch_test {
/** @brief 仅计数的确定性底层唤醒端，不创建线程、IO或实际负载。 */
struct counting_waker {
  std::size_t notifications{};

  void wake_up() noexcept { ++notifications; }
};
}  // namespace scheduler_sleep_batch_test

/** @brief 自然 IO 返回后先撤销 owner 的休眠记录，批次通知只能选择其他睡眠 worker。 */
TEST(SchedulerSleepBatchContract, NaturalIoCompletionNotifiesPeerWithoutSelfWake) {
  using namespace faio::runtime::detail;
  domain_scheduler domain{2};
  scheduler_sleep_batch_test::counting_waker owner_waker, peer_waker;
  local_scheduler owner{domain, 0, 61, worker_waker_ref{owner_waker}};
  local_scheduler peer{domain, 1, 61, worker_waker_ref{peer_waker}};
  auto first = scheduler_test::make_queue_frame(1);
  auto second = scheduler_test::make_queue_frame(2);
  ASSERT_TRUE(peer.prepare_sleep());
  ASSERT_TRUE(owner.prepare_sleep());  // owner 是 sleepers 最后项，旧的提前 flush 会选择自己。
  owner.enqueue_ready(first.handle());
  owner.enqueue_ready(second.handle());
  ASSERT_TRUE(owner.finish_sleep());  // 同一入口覆盖 wait 返回和登记后早期 ready 重查。
  EXPECT_EQ(owner_waker.notifications, 0u);
  EXPECT_EQ(peer_waker.notifications, 1u);
  owner.flush_ready();  // 已有 peer 搜索者时同一批次不能重复通知。
  EXPECT_EQ(peer_waker.notifications, 1u);
  const auto own_task = owner.next_task(1);
  ASSERT_TRUE(own_task);
  EXPECT_EQ(own_task->address(), first.handle().address());
  ASSERT_TRUE(peer.finish_sleep());  // 唤醒方已摘除它，进入既有搜索状态。
  const auto stolen = peer.steal_task();
  ASSERT_TRUE(stolen);
  EXPECT_EQ(stolen->address(), second.handle().address());
  peer.before_execute();  // 归还名额，队列已空，不产生额外通知。
  owner.before_execute();
  EXPECT_EQ(owner_waker.notifications, 0u);
  EXPECT_EQ(peer_waker.notifications, 1u);
}

/** @brief 早期重查只看见一个私有 IO 快速任务时，不唤醒无法窃取它的同伴。 */
TEST(SchedulerSleepBatchContract, EarlyReadyRecheckKeepsPrivateIoTaskOnOwner) {
  using namespace faio::runtime::detail;
  domain_scheduler domain{2};
  scheduler_sleep_batch_test::counting_waker owner_waker, peer_waker;
  local_scheduler owner{domain, 0, 61, worker_waker_ref{owner_waker}};
  local_scheduler peer{domain, 1, 61, worker_waker_ref{peer_waker}};
  auto io = scheduler_test::make_queue_frame(1);
  ASSERT_TRUE(peer.prepare_sleep());
  ASSERT_TRUE(owner.prepare_sleep());
  {
    faio::detail::execution_thread_binding binding{
        faio::scheduler_ref{domain}, {}, owner.local_state(), 0};
    faio::detail::execution_thread_guard guard{binding};
    faio::scheduler_ref{domain}.schedule_io(io.handle());
  }
  ASSERT_TRUE(owner.has_ready_task());  // worker::sleep 的登记后 has_ready_task 分支。
  ASSERT_TRUE(owner.finish_sleep());
  EXPECT_EQ(owner_waker.notifications, 0u);
  EXPECT_EQ(peer_waker.notifications, 0u);
  EXPECT_FALSE(peer.finish_sleep());  // peer 仍在 sleepers，没有被错误摘除或增加搜索计数。
  const auto task = owner.next_task(1);
  ASSERT_TRUE(task);
  EXPECT_EQ(task->address(), io.handle().address());
  owner.before_execute();
}

/** @brief 已被远程唤醒的 owner 保留搜索责任，不重复增加计数或提前重复通知同伴。 */
TEST(SchedulerSleepBatchContract, AlreadyRemotelyNotifiedOwnerKeepsSearchHandoff) {
  using namespace faio::runtime::detail;
  domain_scheduler domain{2};
  scheduler_sleep_batch_test::counting_waker owner_waker, peer_waker;
  local_scheduler owner{domain, 0, 61, worker_waker_ref{owner_waker}};
  local_scheduler peer{domain, 1, 61, worker_waker_ref{peer_waker}};
  auto first = scheduler_test::make_queue_frame(1);
  auto second = scheduler_test::make_queue_frame(2);
  ASSERT_TRUE(peer.prepare_sleep());
  ASSERT_TRUE(owner.prepare_sleep());
  domain.enqueue(first.handle());   // 无 owner 绑定，远程入口选择最后休眠的 owner。
  domain.enqueue(second.handle());  // 已存在搜索者，第二条不能重复唤醒。
  EXPECT_EQ(owner_waker.notifications, 1u);
  EXPECT_EQ(peer_waker.notifications, 0u);
  ASSERT_TRUE(owner.finish_sleep());
  EXPECT_TRUE(owner.is_searching());  // 已摘除分支不能把它当成自行醒来重复登记。
  EXPECT_EQ(owner_waker.notifications, 1u);
  EXPECT_EQ(peer_waker.notifications, 0u);
  const auto own_task = owner.next_task(1);  // 两条全局任务：最后项立即执行、前项留入 FIFO。
  ASSERT_TRUE(own_task);
  EXPECT_EQ(own_task->address(), second.handle().address());
  owner.before_execute();  // 最后搜索者退出时仍按原合同为剩余 FIFO 通知同伴。
  EXPECT_EQ(owner_waker.notifications, 1u);
  EXPECT_EQ(peer_waker.notifications, 1u);
  ASSERT_TRUE(peer.finish_sleep());
  const auto stolen = peer.steal_task();
  ASSERT_TRUE(stolen);
  EXPECT_EQ(stolen->address(), first.handle().address());
  peer.before_execute();
  EXPECT_FALSE(owner.has_ready_task());
  EXPECT_FALSE(peer.has_ready_task());
}

TEST(SchedulerTest, PureCustomSchedulerRunsJoinWithoutRuntime) {
  scheduler_test::manual_scheduler scheduler;
  scheduler_test::manual_lifetime lifetime;
  int local_state = 0;
  std::optional<faio::join_handle<int>> result;
  {
    faio::detail::execution_thread_binding binding{
        faio::scheduler_ref{scheduler}, faio::task_lifetime_ref{lifetime}, local_state, 7};
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
  EXPECT_THROW(faio::detail::start_detached(std::move(root),
                                            faio::scheduler_ref{scheduler},
                                            &tracker,
                                            faio::task_lifetime_ref{lifetime}),
               std::runtime_error);
  tracker.wait();
  EXPECT_EQ(lifetime.active, 0);
  EXPECT_EQ(lifetime.registered, 1);
  EXPECT_EQ(lifetime.finished, 1);
  tracker.add();
  auto rejected = faio::detail::spawn_coro(scheduler_test::budget_loop(), &tracker);
  EXPECT_THROW(faio::detail::start_detached(
                   std::move(rejected), {}, &tracker, faio::task_lifetime_ref{lifetime}),
               std::logic_error);
  tracker.wait();
  EXPECT_EQ(lifetime.registered, 1);  // 空调度器不能归还从未登记的计数。
}

TEST(SchedulerTest, CooperativeBudgetActuallyYieldsEvery64Operations) {
  scheduler_test::manual_scheduler scheduler;
  int local_state = 0;
  faio::detail::execution_thread_binding binding{
      faio::scheduler_ref{scheduler}, {}, local_state, 0};
  faio::detail::execution_thread_guard guard{binding};
  faio::spawn_detached(scheduler_test::budget_loop());
  scheduler.pump();
  EXPECT_EQ(scheduler.submitted, 3u);  // 初次提交 + 第 64 次 + 第 128 次让出。
}

TEST(SchedulerQueueTest, ClosedGlobalQueueRollsBackLocalOverflow) {
  faio::runtime::detail::global_ready_queue global;
  faio::runtime::detail::local_ready_queue<8> local;
  std::vector<scheduler_test::queue_frame> frames;
  for (std::size_t i = 0; i < 10; ++i)
    frames.push_back(scheduler_test::make_queue_frame(i));
  local.push_local(frames[8].handle(), global);
  for (std::size_t i = 0; i < 8; ++i)
    local.push_back(frames[i].handle(), global);
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
  for (std::size_t i = 0; i < count; ++i)
    frames.push_back(scheduler_test::make_queue_frame(i));
  auto seen = std::make_unique<std::atomic<unsigned>[]>(count);
  std::atomic<bool> producer_done{false};
  auto consume = [&](std::coroutine_handle<> handle) {
    seen[scheduler_test::queue_frame::id(handle)].fetch_add(1, std::memory_order_relaxed);
  };
  std::vector<std::jthread> thieves;
  for (int i = 0; i < 3; ++i)
    thieves.emplace_back([&] {
      faio::runtime::detail::local_ready_queue<64> destination;
      for (;;) {
        if (auto task = destination.try_pop()) {
          consume(*task);
          continue;
        }
        if (auto task = source.steal_into(destination)) {
          consume(*task);
          continue;
        }
        if (auto task = global.try_pop()) {
          consume(*task);
          continue;
        }
        if (producer_done.load(std::memory_order_acquire) && source.empty() && global.empty())
          break;
        std::this_thread::yield();
      }
    });
  for (std::size_t i = 0; i < count; ++i) {
    source.push_back(frames[i].handle(), global);
    if (i % 3 == 0)
      if (auto task = source.try_pop())
        consume(*task);
  }
  while (auto task = source.try_pop())
    consume(*task);
  producer_done.store(true, std::memory_order_release);
  thieves.clear();  // join 完成后才读取计数并销毁测试帧。
  for (std::size_t i = 0; i < count; ++i)
    ASSERT_EQ(seen[i].load(std::memory_order_relaxed), 1u) << "frame " << i;
}

TEST(SchedulerTest, CrossRuntimeNotificationReturnsToOriginalDomain) {
  faio::runtime::detail::runtime_context first{
      faio_test::config_builder().set_num_workers(1).build()};
  faio::runtime::detail::runtime_context second{
      faio_test::config_builder().set_num_workers(1).build()};
  faio::sync::semaphore signal{0};
  std::atomic<bool> entered{false}, correct_domain{false};
  auto waiter = first.spawn_observed(
      scheduler_test::mark_entered_and_wait(signal, entered, first.scheduler(), correct_domain));
  first.spawn_observed(scheduler_test::confirm_suspended(entered)).get();
  second.block_on(scheduler_test::release_from_worker(signal));
  waiter.get();
  EXPECT_TRUE(correct_domain.load(std::memory_order_acquire));
}

TEST(SchedulerTest, MultipleProducersYieldAndDrainBeforeShutdown) {
  faio::runtime::detail::runtime_context context{
      faio_test::config_builder().set_num_workers(4).build()};
  std::atomic<std::size_t> completed{0};
  std::vector<std::jthread> producers;
  for (int i = 0; i < 4; ++i)
    producers.emplace_back([&] {
      for (int j = 0; j < 1000; ++j)
        context.submit(scheduler_test::yielding_root(completed));
    });
  producers.clear();
  context.stop();
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 4000u);
}

TEST(SchedulerTest, FastSlotCannotStarveLocalFifoTasks) {
  faio::runtime::detail::runtime_context context{
      faio_test::config_builder().set_num_workers(1).build()};
  EXPECT_LE(context.block_on(scheduler_test::check_fast_slot_fairness()), 4u);
}

TEST(SchedulerTest, IoAndTimerCompletionsUseTheSameReadySink) {
  faio::runtime::detail::runtime_context context{
      faio_test::config_builder().set_num_workers(2).build()};
  scheduler_test::pipe_descriptors pipe;
  auto [value, ignored] =
      context.block_on(faio::join(scheduler_test::read_pipe(pipe.descriptors[0]),
                                  scheduler_test::write_pipe_after_timer(pipe.descriptors[1])));
  (void)ignored;
  EXPECT_EQ(value, 'x');
}

TEST(SchedulerTest, CancelledIoCompletesBeforeRuntimeShutdown) {
  faio::runtime::detail::runtime_context context{
      faio_test::config_builder().set_num_workers(1).build()};
  scheduler_test::pipe_descriptors pipe;
  std::atomic<bool> entered{false};
  auto reader =
      context.spawn_observed(scheduler_test::cancel_pipe_read(pipe.descriptors[0], entered));
  context.spawn_observed(scheduler_test::confirm_suspended(entered)).get();
  reader.request_stop();
  EXPECT_TRUE(reader.get());
  context.stop();
}

TEST(SchedulerTest, ZeroIdleSpinStillHandlesRepeatedExternalNotifications) {
  faio::runtime::detail::runtime_context context{
      faio_test::config_builder().set_num_workers(1).set_idle_spin_count(0).build()};
  faio::sync::semaphore signal{0};
  std::atomic<std::size_t> completed{0};
  constexpr std::size_t count = 1000;
  auto receiver = context.spawn_observed(
      scheduler_test::receive_external_notifications(signal, completed, count));
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
  faio::runtime::detail::runtime_context context{
      faio_test::config_builder().set_num_workers(4).build()};
  faio::sync::semaphore signal{0};
  std::atomic<std::size_t> consumed{0};
  std::vector<faio::join_handle<void>> consumers;
  for (int i = 0; i < 8; ++i)
    consumers.push_back(context.spawn_observed(
        [](faio::sync::semaphore& sem, std::atomic<std::size_t>& count) -> faio::task<void> {
          for (int j = 0; j < 1000; ++j) {
            co_await sem.acquire();
            count.fetch_add(1, std::memory_order_relaxed);
            if (j % 32 == 0)
              co_await faio::this_coro::yield();
          }
        }(signal, consumed)));
  std::vector<std::jthread> producers;
  for (int i = 0; i < 4; ++i)
    producers.emplace_back([&] {
      for (int j = 0; j < 2000; ++j)
        signal.release();
    });
  producers.clear();
  for (auto& handle : consumers)
    handle.get();
  EXPECT_EQ(consumed.load(), 8000u);
  EXPECT_EQ(signal.available_permits(), 0);
}

TEST(SchedulerTest, SemaphoreLastCancelledWaiterRestoresFastRelease) {
  faio::runtime::detail::runtime_context context{
      faio_test::config_builder().set_num_workers(1).build()};
  faio::sync::semaphore signal{0};
  std::atomic<bool> entered{false};
  auto handle = context.spawn_observed(
      [](faio::sync::semaphore& sem, std::atomic<bool>& started) -> faio::task<void> {
        started.store(true, std::memory_order_release);
        co_await sem.acquire();
      }(signal, entered));
  context.spawn_observed(scheduler_test::confirm_suspended(entered)).get();
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

TEST(SchedulerTest, CustomSchedulerResumeRefreshesSharedCooperativeBudget) {
  scheduler_test::manual_scheduler scheduler;
  scheduler_test::manual_lifetime lifetime;
  int local_state{};
  faio::detail::execution_thread_binding binding{
      faio::scheduler_ref{scheduler}, faio::task_lifetime_ref{lifetime}, local_state, 7};
  auto result = [&] {
    faio::detail::execution_thread_guard guard{binding};
    auto submitted = faio::spawn(scheduler_test::manual_budget_after_actual_resume());
    scheduler.pump();
    return submitted;
  }();  // 先退出执行线程绑定，再由外部线程领取已完成结果，保持禁止 worker 阻塞的合同。
  result.get();
  EXPECT_EQ(scheduler.submitted,
            2u);  // 首次 root + explicit yield，不能再多一次 conditional yield。
  EXPECT_EQ(lifetime.active, 0);
}

TEST(SchedulerTest, NestedPollScopesAndExternalBlockOnRestoreOuterRemainingBudget) {
  const auto outside = faio::detail::current_cooperative_budget;
  {
    faio::detail::cooperative_poll_scope outer;
    faio::detail::current_cooperative_budget = 7;  // 模拟外层在进入内层前已消费的额度。
    {
      faio::detail::cooperative_poll_scope inner;
      EXPECT_EQ(faio::detail::current_cooperative_budget, 64u);
      faio::detail::current_cooperative_budget = 3;
    }
    EXPECT_EQ(faio::detail::current_cooperative_budget, 7u);
    try {
      faio::detail::cooperative_poll_scope exceptional;
      faio::detail::current_cooperative_budget = 2;
      throw std::runtime_error("内层执行异常");
    } catch (const std::runtime_error&) {
    }
    EXPECT_EQ(faio::detail::current_cooperative_budget, 7u);
    // 此处没有 runtime worker 绑定；允许外部 block_on，内部多个 poll 不能偷走外层额度。
    faio::runtime::detail::runtime_context runtime{
        faio_test::config_builder().set_mode(faio::runtime::mode::current_thread).build()};
    EXPECT_EQ(runtime.block_on(scheduler_test::manual_value(42)), 42);
    EXPECT_EQ(faio::detail::current_cooperative_budget, 7u);
  }
  EXPECT_EQ(faio::detail::current_cooperative_budget, outside);
}

namespace external_awaiter_test {
/** @brief 外部 xvalue 等待者的生命周期观察器，不拥有测试 payload。 */
struct observation {
  std::coroutine_handle<> pending{};  // 只由 peer 领取并重新提交一次。
  unsigned source_destroyed{0};       // 外部原对象在挂起间隙已真正销毁。
  unsigned owners_destroyed{0};       // 持有 payload 的帧内 awaiter 恰好销毁一次。
  bool peer_saw_payload{false};       // peer 通过 weak witness 读取真实 payload。
  bool resume_saw_payload{false};     // 恢复必须从 awaiter 自己的所有权读取数据。
};

/** @brief 移动时将内部 payload 消费给新对象；外部源对象可在挂起后销毁。 */
struct owning_awaiter {
  observation& state;
  std::shared_ptr<int> payload;  // 原始唯一强引用；weak witness 不延长生命周期。
  bool source{true};             // 区分原始外部对象与接管 payload 的帧内对象。

  explicit owning_awaiter(observation& observed)
      : state(observed), payload(std::make_shared<int>(42)) {}

  owning_awaiter(const owning_awaiter&) = delete;

  owning_awaiter& operator=(const owning_awaiter&) = delete;

  owning_awaiter(owning_awaiter&& other) noexcept
      : state(other.state), payload(std::move(other.payload)), source(false) {}

  owning_awaiter& operator=(owning_awaiter&&) = delete;

  ~owning_awaiter() {
    if (source)
      ++state.source_destroyed;
    if (payload)
      ++state.owners_destroyed;
  }

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> continuation) noexcept {
    state.pending = continuation;  // 确保控制流先回到真实 scheduler 边界。
  }

  int await_resume() {
    state.resume_saw_payload = static_cast<bool>(payload);
    if (!payload)
      throw std::logic_error("恢复时丢失 awaiter 的 payload 所有权");
    return *payload;  // peer 的临时强引用已归还，只能依靠当前对象所有权。
  }
};

faio::task<int> consume_external(owning_awaiter& external) {
  co_return co_await std::move(external);  // 公共协议目前保证 xvalue 被移动进帧。
}

/** @brief 先检查外部对象确实销毁后的 payload，再独立安排等待者下一轮恢复。 */
faio::task<void> observe_and_resume(observation& state,
                                    const std::weak_ptr<int>& witness,
                                    scheduler_test::manual_scheduler& scheduler) {
  {
    const auto payload = witness.lock();  // 挂起间隙才临时观察，不替代帧内所有权。
    state.peer_saw_payload = state.source_destroyed == 1 && payload && *payload == 42;
  }  // 在续体入队前归还 peer 的强引用，避免掩盖所有权丢失。
  const auto continuation = std::exchange(state.pending, {});
  if (continuation)
    scheduler.enqueue(continuation);
  co_return;
}

/** @brief 测试专用帧所有者；没有内核或等待队列借用此手工 awaiter 的地址。 */
struct frame_owner {
  faio::task<int>::handle_type handle;
  observation& state;

  ~frame_owner() {
    state.pending = {};  // 失败分支先撤去唯一手工借用，再销毁帧。
    if (handle)
      handle.destroy();
  }
};
}  // namespace external_awaiter_test

TEST(AwaiterOwnershipTest, MovedExternalAwaiterOwnsPayloadAfterSourceDestruction) {
  external_awaiter_test::observation state;
  scheduler_test::manual_scheduler scheduler;
  auto external = std::make_unique<external_awaiter_test::owning_awaiter>(state);
  const std::weak_ptr<int> witness = external->payload;
  auto task = external_awaiter_test::consume_external(*external);
  external_awaiter_test::frame_owner frame{task.take(), state};
  frame.handle.promise().context.scheduler = faio::scheduler_ref{scheduler};
  int local_state = 0;
  std::optional<faio::join_handle<void>> peer;
  {
    faio::detail::execution_thread_binding binding{
        faio::scheduler_ref{scheduler}, {}, local_state, 0};
    faio::detail::execution_thread_guard guard{binding};
    {
      faio::detail::cooperative_poll_scope poll_budget;
      frame.handle.resume();  // 初次真实恢复必须执行到 await_suspend 并返回。
    }
    ASSERT_EQ(state.pending.address(), frame.handle.address());
    EXPECT_FALSE(static_cast<bool>(external->payload));  // xvalue 已消费给帧内对象。
    external.reset();                                    // 外部持有者在挂起间隙销毁原对象。
    EXPECT_EQ(state.source_destroyed, 1u);
    EXPECT_FALSE(witness.expired());  // awaiter 的 payload 必须继续由任务帧持有。
    if (witness.expired())
      return;  // 借用回归时不恢复悬空对象，测试自身不引入 UB。
    peer.emplace(faio::spawn(external_awaiter_test::observe_and_resume(state, witness, scheduler)));
    scheduler.pump();  // peer 观察之后再以独立一次 resume 领取 payload。
  }
  ASSERT_TRUE(peer->done());  // 所有 get 之前先非阻塞确认，失败不会永久等待。
  peer->get();
  ASSERT_TRUE(frame.handle.done());
  EXPECT_EQ(frame.handle.promise().take_result(), 42);
  EXPECT_TRUE(state.peer_saw_payload);
  EXPECT_TRUE(state.resume_saw_payload);
  EXPECT_EQ(state.owners_destroyed, 1u);
  EXPECT_TRUE(witness.expired());      // 完整表达式结束后没有残留强引用。
  EXPECT_EQ(scheduler.submitted, 2u);  // peer 首次提交 + peer 提交等待者续体。
}

/** @brief 主动 yield 必须让已经等待的 FIFO 任务成为下一个本地任务。 */
TEST(SchedulerYieldContract, ExplicitYieldPutsWaitingFifoTaskBeforeSelf) {
  using namespace faio::runtime::detail;
  domain_scheduler domain{1};
  domain_scheduler::local_queue_type queue;
  auto waiting = scheduler_test::make_queue_frame(1);
  auto yielding = scheduler_test::make_queue_frame(2);
  faio::detail::execution_thread_binding binding{faio::scheduler_ref{domain}, {}, queue, 0};
  faio::detail::execution_thread_guard guard{binding};
  queue.push_back(waiting.handle(), domain.global_queue());
  std::stop_token stop;
  faio::detail::yield_awaiter operation{faio::scheduler_ref{domain}, nullptr, &stop};
  EXPECT_FALSE(operation.await_ready());
  operation.await_suspend(yielding.handle());
  const auto next = queue.try_pop_local();
  ASSERT_TRUE(next);
  EXPECT_EQ(next->address(), waiting.handle().address());  // 严格下一次；不是放宽到若干 fast 轮。
  const auto resumed = queue.try_pop_local();
  ASSERT_TRUE(resumed);
  EXPECT_EQ(resumed->address(), yielding.handle().address());
  EXPECT_TRUE(queue.empty_local());
}

/** @brief 真实耗尽预算以后等待 FIFO 必须下一次获得执行机会。 */
TEST(SchedulerYieldContract, ExhaustedBudgetPutsWaitingFifoTaskBeforeSelf) {
  using namespace faio::runtime::detail;
  domain_scheduler domain{1};
  domain_scheduler::local_queue_type queue;
  auto waiting = scheduler_test::make_queue_frame(1);
  auto yielding = scheduler_test::make_queue_frame(2);
  faio::detail::execution_thread_binding binding{faio::scheduler_ref{domain}, {}, queue, 0};
  faio::detail::execution_thread_guard guard{binding};
  queue.push_back(waiting.handle(), domain.global_queue());
  faio::detail::task_context context;
  context.scheduler = faio::scheduler_ref{domain};
  faio::detail::cooperative_poll_scope poll;
  faio::detail::current_cooperative_budget = 1;  // 本轮最后一次条件检查，不能提前补充额度。
  faio::detail::yield_if_needed_awaiter operation{&context};
  EXPECT_FALSE(operation.await_ready());
  EXPECT_EQ(faio::detail::current_cooperative_budget, 0u);
  operation.await_suspend(yielding.handle());
  const auto next = queue.try_pop_local();
  ASSERT_TRUE(next);
  EXPECT_EQ(next->address(), waiting.handle().address());
  const auto resumed = queue.try_pop_local();
  ASSERT_TRUE(resumed);
  EXPECT_EQ(resumed->address(), yielding.handle().address());
  EXPECT_TRUE(queue.empty_local());
}

/** @brief 旧 custom scheduler 只需 enqueue，失败时新句柄仍归调用者所有。 */
TEST(SchedulerYieldContract, CustomFallbackAndRejectionPreserveOwnership) {
  scheduler_test::manual_scheduler scheduler;
  auto accepted = scheduler_test::make_queue_frame(1);
  auto rejected = scheduler_test::make_queue_frame(2);
  const faio::scheduler_ref ref{scheduler};
  ref.schedule_yield(accepted.handle());
  ASSERT_EQ(scheduler.ready.size(), 1u);
  EXPECT_EQ(scheduler.ready.front().address(), accepted.handle().address());
  scheduler.reject = true;
  EXPECT_THROW(ref.schedule_yield(rejected.handle()), std::runtime_error);
  EXPECT_EQ(scheduler.submitted, 1u);
  ASSERT_EQ(scheduler.ready.size(), 1u);
  EXPECT_EQ(scheduler.ready.front().address(), accepted.handle().address());
  scheduler.ready.clear();  // 两帧仍由 queue_frame 独占；测试没有恢复或移交销毁责任。
  EXPECT_THROW(faio::scheduler_ref{}.schedule_yield(rejected.handle()), std::logic_error);
}

namespace scheduler_yield_test {
/** @brief 两条真实 task 挂起路径用于检查失败没有泄漏根任务/生命周期计数。 */
faio::task<void> rejected_yield(bool conditional) {
  if (conditional) {
    for (unsigned count = 0; count < faio::detail::cooperative_budget_limit; ++count)
      co_await faio::this_coro::yield_if_needed();
  } else {
    co_await faio::this_coro::yield();
  }
}
}  // namespace scheduler_yield_test

TEST(SchedulerYieldContract, RejectedYieldCompletesRootAndReturnsLifetimeCounts) {
  for (const bool conditional : {false, true}) {
    scheduler_test::manual_scheduler scheduler;
    scheduler_test::manual_lifetime lifetime;
    int local_state{};
    std::optional<faio::join_handle<void>> result;
    {
      faio::detail::execution_thread_binding binding{
          faio::scheduler_ref{scheduler}, faio::task_lifetime_ref{lifetime}, local_state, 0};
      faio::detail::execution_thread_guard guard{binding};
      result.emplace(faio::spawn(scheduler_yield_test::rejected_yield(conditional)));
      EXPECT_EQ(lifetime.active, 1);
      scheduler.reject = true;  // 初次提交成功；只拒绝 task 内实际发生的让出重新入队。
      scheduler.pump();
      EXPECT_EQ(lifetime.active, 0);
      EXPECT_EQ(lifetime.registered, lifetime.finished);
      EXPECT_TRUE(scheduler.ready.empty());
    }  // 退出 worker 绑定后才能调用 get，维持原有禁止 worker 阻塞合同。
    ASSERT_TRUE(result->done());
    EXPECT_THROW(result->get(), std::runtime_error);
  }
}

/** @brief 外部让出投递遵守全局队列关闭合同，不保存被拒绝的续体。 */
TEST(SchedulerYieldContract, ClosedGlobalQueueRejectsExternalYield) {
  faio::runtime::detail::domain_scheduler domain{1};
  auto rejected = scheduler_test::make_queue_frame(1);
  domain.close();
  EXPECT_THROW(faio::scheduler_ref{domain}.schedule_yield(rejected.handle()), std::logic_error);
  EXPECT_TRUE(domain.global_queue().empty());
  EXPECT_FALSE(rejected.handle().done());  // 未在 enqueue 内恢复，也没有销毁调用者的帧。
}

/** @brief 本地满队列的 FIFO 溢出失败，必须完整保留此前句柄和发布边界。 */
TEST(SchedulerYieldContract, ClosedOverflowPreservesAllWaitingFifoHandles) {
  using namespace faio::runtime::detail;
  domain_scheduler domain{1};
  domain_scheduler::local_queue_type queue;
  std::vector<scheduler_test::queue_frame> frames;
  frames.reserve(LOCAL_QUEUE_CAPACITY + 1);
  for (std::size_t id = 0; id <= LOCAL_QUEUE_CAPACITY; ++id)
    frames.push_back(scheduler_test::make_queue_frame(id));
  faio::detail::execution_thread_binding binding{faio::scheduler_ref{domain}, {}, queue, 0};
  faio::detail::execution_thread_guard guard{binding};
  for (std::size_t id = 0; id < LOCAL_QUEUE_CAPACITY; ++id)
    queue.push_back(frames[id].handle(), domain.global_queue());
  domain.global_queue().close();
  EXPECT_THROW(faio::scheduler_ref{domain}.schedule_yield(frames.back().handle()),
               std::logic_error);
  EXPECT_TRUE(domain.global_queue().empty());
  for (std::size_t id = 0; id < LOCAL_QUEUE_CAPACITY; ++id) {
    const auto task = queue.try_pop_local();
    ASSERT_TRUE(task);
    EXPECT_EQ(task->address(), frames[id].handle().address());
  }
  EXPECT_TRUE(queue.empty_local());
}

/** @brief 主动让出不改变正常 IO 完成的快速槽语义，也不能覆盖该私有任务。 */
TEST(SchedulerYieldContract, IoFastSlotRemainsAvailableWhenYieldGoesToFifo) {
  using namespace faio::runtime::detail;
  domain_scheduler domain{1};
  domain_scheduler::local_queue_type queue;
  auto waiting = scheduler_test::make_queue_frame(1);
  auto io = scheduler_test::make_queue_frame(2);
  auto yielding = scheduler_test::make_queue_frame(3);
  faio::detail::execution_thread_binding binding{faio::scheduler_ref{domain}, {}, queue, 0};
  faio::detail::execution_thread_guard guard{binding};
  queue.push_back(waiting.handle(), domain.global_queue());
  const faio::scheduler_ref ref{domain};
  ref.schedule_io(io.handle());
  ref.schedule_yield(yielding.handle());
  const auto next = queue.try_pop_local();
  ASSERT_TRUE(next);
  EXPECT_EQ(next->address(), io.handle().address());  // 同域 IO 仍优先使用既有 fast slot。
  const auto fifo = queue.try_pop_local();
  ASSERT_TRUE(fifo);
  EXPECT_EQ(fifo->address(), waiting.handle().address());
  const auto resumed = queue.try_pop_local();
  ASSERT_TRUE(resumed);
  EXPECT_EQ(resumed->address(), yielding.handle().address());
  EXPECT_TRUE(queue.empty_local());
}

namespace scheduler_cooperative_hint_test {
/** @brief 通过真实库 awaiter 提供窄提示，不从测试中访问 scheduler_ref 的私有入口。 */
void publish_self(faio::scheduler_ref scheduler,
                  std::coroutine_handle<> handle,
                  bool conditional = false) {
  if (conditional) {
    faio::detail::task_context context;
    context.scheduler = scheduler;
    faio::detail::cooperative_poll_scope poll;
    faio::detail::current_cooperative_budget = 1;  // 本轮最后检查，必须实际进入 await_suspend。
    faio::detail::yield_if_needed_awaiter operation{&context};
    EXPECT_FALSE(operation.await_ready());
    operation.await_suspend(handle);
  } else {
    std::stop_token stop;
    faio::detail::yield_awaiter operation{scheduler, nullptr, &stop};
    EXPECT_FALSE(operation.await_ready());
    operation.await_suspend(handle);
  }
}

/** @brief 确定性通知对象，原 owner 正工作且 peer 登记休眠；不依赖时钟或线程抢占。 */
struct notification_fixture {
  faio::runtime::detail::domain_scheduler domain{2};
  scheduler_sleep_batch_test::counting_waker owner_waker, peer_waker;
  faio::runtime::detail::local_scheduler owner{
      domain, 0, 61, faio::runtime::detail::worker_waker_ref{owner_waker}};
  faio::runtime::detail::local_scheduler peer{
      domain, 1, 61, faio::runtime::detail::worker_waker_ref{peer_waker}};
};

/** @brief 旧扩展只实现 enqueue_yield；新入口必须保持它的 FIFO/拒绝提交能力。 */
struct legacy_yield_scheduler {
  scheduler_test::manual_scheduler queue;
  unsigned ordinary{}, yielded{};

  void enqueue(std::coroutine_handle<> handle) {
    queue.enqueue(handle);
    ++ordinary;
  }

  void enqueue_yield(std::coroutine_handle<> handle) {
    queue.enqueue(handle);
    ++yielded;
  }
};

/** @brief 显式实现窄能力，确认手工调用与两条库内自让出不会串错静态操作表。 */
struct hinted_scheduler {
  scheduler_test::manual_scheduler queue;
  unsigned ordinary{}, manual{}, cooperative{};

  void enqueue(std::coroutine_handle<> handle) {
    queue.enqueue(handle);
    ++ordinary;
  }

  void enqueue_yield(std::coroutine_handle<> handle) {
    queue.enqueue(handle);
    ++manual;
  }

  void enqueue_cooperative_yield(std::coroutine_handle<> handle) {
    queue.enqueue(handle);
    ++cooperative;
  }
};

/** @brief 单个真实 task 在没有其他就绪工作时必须依靠 owner 自己完成全部让出。 */
faio::task<unsigned> sole_progress(bool conditional) {
  const unsigned checks = conditional ? 8 * faio::detail::cooperative_budget_limit : 8;
  for (unsigned turn = 0; turn < checks; ++turn) {
    if (conditional)
      co_await faio::this_coro::yield_if_needed();
    else
      co_await faio::this_coro::yield();
  }
  co_return checks;
}

/** @brief 使用原子标记避免多 worker 数据竞争，检验 peer 在有界让出期间得到执行。 */
faio::task<bool> peer_progress(bool conditional) {
  std::atomic<bool> ran{false};
  auto peer = faio::spawn([](std::atomic<bool>& marker) -> faio::task<void> {
    marker.store(true, std::memory_order_release);
    co_return;
  }(ran));
  for (unsigned turn = 0; turn < 256 && !ran.load(std::memory_order_acquire); ++turn) {
    if (conditional)
      co_await faio::this_coro::yield_if_needed();
    else
      co_await faio::this_coro::yield();
  }
  const bool observed = ran.load(std::memory_order_acquire);
  co_await peer;  // 两条帧排空后局部标记才析构；失败也不留下借用。
  co_return observed;
}

/** @brief 四worker子任务等待明确start，再执行固定次数并发布一次done通知。 */
faio::task<void> gated_parallel_peer(bool conditional,
                                     faio::sync::semaphore& start,
                                     faio::sync::semaphore& done,
                                     std::atomic<unsigned>& checks,
                                     std::atomic<unsigned>& notifications) {
  co_await start.acquire();  // 先发布许可与先登记等待都合法，不要求哪个CPU先获得时间片。
  for (unsigned turn = 0; turn < 2 * faio::detail::cooperative_budget_limit; ++turn) {
    checks.fetch_add(1, std::memory_order_relaxed);  // 每次实际执行计数，不通过观察标记延长循环。
    if (conditional)
      co_await faio::this_coro::yield_if_needed();
    else
      co_await faio::this_coro::yield();
  }
  notifications.fetch_add(1, std::memory_order_relaxed);
  done.release();  // 固定工作完成才发布唯一许可；父任务仍需join确认根包装完成。
}

/** @brief 四worker验证有界完整工作、通知消费与join；不以父检查次数推断其他CPU的先后。 */
faio::task<std::array<unsigned, 6>> bounded_parallel_progress(bool conditional) {
  faio::sync::semaphore start{0}, done{0};
  std::atomic<unsigned> child_checks{0}, done_notifications{0};
  unsigned parent_checks = 0, start_notifications = 0;
  auto peer =
      faio::spawn(gated_parallel_peer(conditional, start, done, child_checks, done_notifications));
  for (unsigned turn = 0; turn < 4 * faio::detail::cooperative_budget_limit; ++turn) {
    ++parent_checks;  // 固定256次工作，既不重试观察也不放大原检查界限。
    if (conditional)
      co_await faio::this_coro::yield_if_needed();
    else
      co_await faio::this_coro::yield();
  }
  ++start_notifications;
  start.release();          // 父固定工作结束后恰好一次启动；不存在等待OS抢先执行的假设。
  co_await done.acquire();  // 必须消费子任务发布的真实许可，不能把join后标记直接改为true。
  co_await peer;            // 两帧完全排空后读取结果并析构被借用的信号量/计数器。
  co_return std::array<unsigned, 6>{parent_checks,
                                    child_checks.load(std::memory_order_relaxed),
                                    start_notifications,
                                    done_notifications.load(std::memory_order_relaxed),
                                    static_cast<unsigned>(start.available_permits()),
                                    static_cast<unsigned>(done.available_permits())};
}
}  // namespace scheduler_cooperative_hint_test

/** @brief 手工投递另一帧以后当前调用者还会继续执行，通知必须在返回时已经发生。 */
TEST(SchedulerCooperativeHint, ManualYieldNotifiesBeforeCallerContinuesComputing) {
  using namespace scheduler_cooperative_hint_test;
  notification_fixture fixture;
  auto other = scheduler_test::make_queue_frame(0);
  ASSERT_TRUE(fixture.peer.prepare_sleep());
  faio::detail::execution_thread_binding binding{
      faio::scheduler_ref{fixture.domain}, {}, fixture.owner.local_state(), 0};
  faio::detail::execution_thread_guard guard{binding};
  faio::scheduler_ref{fixture.domain}.schedule_yield(other.handle());
  ASSERT_EQ(fixture.peer_waker.notifications, 1u);  // 尚未归还 owner 执行权，已承担原通知责任。
  std::atomic<unsigned> continuing_work{0};
  for (unsigned step = 0; step < 1024; ++step)
    continuing_work.fetch_add(
        1, std::memory_order_relaxed);  // 当前调用仍继续运行，不能借用自让出提示。
  EXPECT_EQ(continuing_work.load(std::memory_order_relaxed), 1024u);
  EXPECT_TRUE(fixture.peer.finish_sleep());
  const auto stolen = fixture.peer.steal_task();
  ASSERT_TRUE(stolen);
  EXPECT_EQ(stolen->address(), other.handle().address());  // 原 FIFO 仍可被已唤醒同伴领取。
  fixture.peer.before_execute();
  EXPECT_FALSE(fixture.owner.has_ready_task());
}

/** @brief 显式/预算让出的唯一续体不唤醒同伴，owner 仍取得唯一 FIFO 句柄。 */
TEST(SchedulerCooperativeHint, SoleSelfYieldStaysReadyWithoutPeerNotification) {
  using namespace scheduler_cooperative_hint_test;
  for (const bool conditional : {false, true}) {
    notification_fixture fixture;
    auto yielding = scheduler_test::make_queue_frame(0);
    ASSERT_TRUE(fixture.peer.prepare_sleep());
    faio::detail::execution_thread_binding binding{
        faio::scheduler_ref{fixture.domain}, {}, fixture.owner.local_state(), 0};
    faio::detail::execution_thread_guard guard{binding};
    publish_self(faio::scheduler_ref{fixture.domain}, yielding.handle(), conditional);
    EXPECT_EQ(fixture.peer_waker.notifications, 0u);
    EXPECT_TRUE(fixture.domain.still_sleeping(1));
    const auto next = fixture.owner.next_task(1);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->address(), yielding.handle().address());
    EXPECT_FALSE(fixture.owner.has_ready_task());
    EXPECT_TRUE(fixture.domain.cancel_sleep(1));
  }
}

/** @brief 已有 FIFO、fast 或 global 工作时，窄提示仍负责通知且不丢失任何句柄。 */
TEST(SchedulerCooperativeHint, ExistingFifoFastOrGlobalWorkStillNotifiesPeer) {
  using namespace scheduler_cooperative_hint_test;
  for (unsigned source = 0; source < 3; ++source) {
    notification_fixture fixture;
    auto waiting = scheduler_test::make_queue_frame(0);
    auto yielding = scheduler_test::make_queue_frame(1);
    ASSERT_TRUE(fixture.peer.prepare_sleep());
    auto& queue = fixture.owner.local_state();
    if (source == 0)
      queue.push_back(waiting.handle(), fixture.domain.global_queue());
    else if (source == 1)
      queue.push_local(waiting.handle(), fixture.domain.global_queue());
    else
      fixture.domain.global_queue().push_back(
          waiting.handle());  // 不调用通知，仅准备原全局 backlog。
    faio::detail::execution_thread_binding binding{
        faio::scheduler_ref{fixture.domain}, {}, queue, 0};
    faio::detail::execution_thread_guard guard{binding};
    publish_self(faio::scheduler_ref{fixture.domain}, yielding.handle());
    EXPECT_EQ(fixture.peer_waker.notifications, 1u);
    std::array<unsigned, 2> seen{};
    while (auto task = fixture.domain.global_queue().try_pop())
      ++seen[scheduler_test::queue_frame::id(*task)];
    while (auto task = queue.try_pop_local())
      ++seen[scheduler_test::queue_frame::id(*task)];
    EXPECT_EQ(seen[0], 1u);
    EXPECT_EQ(seen[1], 1u);
    EXPECT_TRUE(fixture.peer.finish_sleep());
    fixture.peer.before_execute();
  }
}

/** @brief 没有匹配 owner 的外部/跨域提示仍进入全局队列并承担无条件通知。 */
TEST(SchedulerCooperativeHint, RemoteAndForeignBindingRetainNotification) {
  using namespace scheduler_cooperative_hint_test;
  for (const bool foreign : {false, true}) {
    notification_fixture fixture;
    faio::runtime::detail::domain_scheduler other_domain{1};
    faio::runtime::detail::domain_scheduler::local_queue_type other_queue;
    auto yielding = scheduler_test::make_queue_frame(0);
    ASSERT_TRUE(fixture.peer.prepare_sleep());
    if (foreign) {
      faio::detail::execution_thread_binding binding{
          faio::scheduler_ref{other_domain}, {}, other_queue, 0};
      faio::detail::execution_thread_guard guard{binding};
      publish_self(faio::scheduler_ref{fixture.domain}, yielding.handle());
    } else
      publish_self(faio::scheduler_ref{fixture.domain}, yielding.handle());
    EXPECT_EQ(fixture.peer_waker.notifications, 1u);
    EXPECT_FALSE(fixture.owner.has_local_task());
    const auto task = fixture.domain.global_queue().try_pop();
    ASSERT_TRUE(task);
    EXPECT_EQ(task->address(), yielding.handle().address());
    EXPECT_TRUE(fixture.peer.finish_sleep());
    fixture.peer.before_execute();
  }
}

/** @brief 新能力仍保留满 FIFO 的批量转交与拒绝提交回滚，不能省略容量失败清理。 */
TEST(SchedulerCooperativeHint, OverflowNotifiesAndRejectedOverflowPreservesOwnership) {
  using namespace scheduler_cooperative_hint_test;
  using namespace faio::runtime::detail;
  for (const bool reject : {false, true}) {
    notification_fixture fixture;
    std::vector<scheduler_test::queue_frame> frames;
    frames.reserve(LOCAL_QUEUE_CAPACITY + 1);
    for (unsigned id = 0; id <= LOCAL_QUEUE_CAPACITY; ++id)
      frames.push_back(scheduler_test::make_queue_frame(id));
    ASSERT_TRUE(fixture.peer.prepare_sleep());
    auto& queue = fixture.owner.local_state();
    for (unsigned id = 0; id < LOCAL_QUEUE_CAPACITY; ++id)
      queue.push_back(frames[id].handle(), fixture.domain.global_queue());
    faio::detail::execution_thread_binding binding{
        faio::scheduler_ref{fixture.domain}, {}, queue, 0};
    faio::detail::execution_thread_guard guard{binding};
    if (reject) {
      fixture.domain.global_queue()
          .close();  // 不调用 domain.close 的广播，以便单独验证失败不通知。
      EXPECT_THROW(publish_self(faio::scheduler_ref{fixture.domain}, frames.back().handle()),
                   std::logic_error);
      EXPECT_EQ(fixture.peer_waker.notifications, 0u);
      EXPECT_FALSE(frames.back().handle().done());
    } else {
      publish_self(faio::scheduler_ref{fixture.domain}, frames.back().handle());
      EXPECT_EQ(fixture.peer_waker.notifications, 1u);
      EXPECT_FALSE(fixture.domain.global_queue().empty());  // 原溢出真实转交半 FIFO 加新任务。
    }
    std::vector<unsigned> seen(LOCAL_QUEUE_CAPACITY + 1);
    while (auto task = fixture.domain.global_queue().try_pop())
      ++seen[scheduler_test::queue_frame::id(*task)];
    while (auto task = queue.try_pop_local())
      ++seen[scheduler_test::queue_frame::id(*task)];
    for (unsigned id = 0; id < LOCAL_QUEUE_CAPACITY; ++id)
      EXPECT_EQ(seen[id], 1u);
    EXPECT_EQ(seen.back(), reject ? 0u : 1u);
    if (reject)
      EXPECT_TRUE(fixture.domain.cancel_sleep(1));
    else {
      EXPECT_TRUE(fixture.peer.finish_sleep());
      fixture.peer.before_execute();
    }
  }
}

/** @brief 可选能力严格回退旧 yield，而手工公开方法不会误传 self-returning 提示。 */
TEST(SchedulerCooperativeHint, CustomFallbackSelectionAndRejectionRemainExact) {
  using namespace scheduler_cooperative_hint_test;
  auto first = scheduler_test::make_queue_frame(0);
  auto second = scheduler_test::make_queue_frame(1);
  auto third = scheduler_test::make_queue_frame(2);
  scheduler_test::manual_scheduler plain;
  publish_self(faio::scheduler_ref{plain}, first.handle());
  ASSERT_EQ(plain.ready.size(), 1u);
  EXPECT_EQ(plain.ready.front().address(), first.handle().address());
  plain.ready.clear();
  plain.reject = true;
  EXPECT_THROW(publish_self(faio::scheduler_ref{plain}, second.handle()), std::runtime_error);
  EXPECT_TRUE(plain.ready.empty());
  legacy_yield_scheduler legacy;
  publish_self(faio::scheduler_ref{legacy}, first.handle());
  publish_self(faio::scheduler_ref{legacy}, second.handle(), true);
  EXPECT_EQ(legacy.ordinary, 0u);
  EXPECT_EQ(legacy.yielded, 2u);
  legacy.queue.ready.clear();
  legacy.queue.reject = true;
  EXPECT_THROW(publish_self(faio::scheduler_ref{legacy}, third.handle()), std::runtime_error);
  EXPECT_TRUE(legacy.queue.ready.empty());
  hinted_scheduler hinted;
  const faio::scheduler_ref ref{hinted};
  ref.schedule_yield(first.handle());  // 公开手工入口保留原能力，不能使用窄回调。
  publish_self(ref, second.handle());
  publish_self(ref, third.handle(), true);
  EXPECT_EQ(hinted.ordinary, 0u);
  EXPECT_EQ(hinted.manual, 1u);
  EXPECT_EQ(hinted.cooperative, 2u);
  EXPECT_EQ(hinted.queue.ready.size(), 3u);
  hinted.queue.ready.clear();
  hinted.queue.reject = true;
  EXPECT_THROW(publish_self(ref, third.handle()), std::runtime_error);
  EXPECT_TRUE(hinted.queue.ready.empty());
  EXPECT_THROW(publish_self({}, third.handle()), std::logic_error);
  EXPECT_FALSE(third.handle().done());
}

/** @brief 真实一/四 worker 恢复验证 sole 进度与显式/预算让出公平性，不使用时延阈值。 */
TEST(SchedulerCooperativeHint, ActualRuntimeCompletesSoleYieldsAndDoesNotStarvePeer) {
  using namespace scheduler_cooperative_hint_test;
  for (const unsigned workers : {1u, 4u}) {
    faio::runtime::detail::runtime_context runtime{
        faio_test::config_builder().set_num_workers(workers).build()};
    for (const bool conditional : {false, true}) {
      SCOPED_TRACE(::testing::Message{} << "workers=" << workers << " conditional=" << conditional);
      EXPECT_EQ(runtime.block_on(sole_progress(conditional)),
                conditional ? 8u * faio::detail::cooperative_budget_limit : 8u);
      if (workers == 1) {
        EXPECT_TRUE(
            runtime.block_on(peer_progress(conditional)));  // 单worker原join前有界观测断言保留。
      } else {
        const auto completed = runtime.block_on(bounded_parallel_progress(conditional));
        const std::array<unsigned, 6> expected{4u * faio::detail::cooperative_budget_limit,
                                               2u * faio::detail::cooperative_budget_limit,
                                               1u,
                                               1u,
                                               0u,
                                               0u};
        EXPECT_EQ(completed, expected);  // 有界完整次数、各一次通知、许可消费与join排空逐项验证。
      }
    }
  }
}

namespace frame_allocator_test {
/** @brief 手工帧的独占所有者；等待只交给测试，不存在内核/队列地址借用。 */
template <class T>
struct owned_frame {
  typename faio::task<T>::handle_type handle;

  explicit owned_frame(faio::task<T> task) noexcept : handle(task.take()) {}

  owned_frame(const owned_frame&) = delete;

  ~owned_frame() {
    if (handle)
      handle.destroy();
  }
};

// 线程退出时晚于运行栈 cache scope 析构，必须走 global fallback。
thread_local std::optional<faio::task<int>> late_task;

/** @brief promise 构造故意抛异常，由编译器调用标准 delete 回收已分配 storage。 */
struct construction_probe {
  struct promise_type : faio::detail::task_frame_allocation<promise_type> {
    explicit promise_type(bool fail) {
      if (fail)
        throw std::runtime_error("promise 构造测试异常");
    }

    construction_probe get_return_object() noexcept {
      return construction_probe{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() const noexcept { return {}; }

    std::suspend_always final_suspend() const noexcept { return {}; }

    void return_void() const noexcept {}

    void unhandled_exception() const noexcept { std::terminate(); }
  };

  std::coroutine_handle<promise_type> handle;

  explicit construction_probe(std::coroutine_handle<promise_type> value) noexcept : handle(value) {}

  construction_probe(const construction_probe&) = delete;

  construction_probe(construction_probe&& other) noexcept
      : handle(std::exchange(other.handle, {})) {}

  ~construction_probe() {
    if (handle)
      handle.destroy();
  }
};

// GCC noinline 仍允许 IPA 常量传播克隆；此工厂用于验证实际分配后的异常清理。
#if defined(__GNUC__) && !defined(__clang__)
[[gnu::noipa]]
#else
[[gnu::noinline]]
#endif
construction_probe construct_probe(bool fail) {
  (void)fail;
  co_return;
}
}  // namespace frame_allocator_test

TEST(CoroutineFrameCacheTest, HeaderAndTlsUseOneIdentityAcrossTranslationUnits) {
  EXPECT_EQ(frame_allocator_test::other_tu_tls_address(), &faio::detail::current_frame_cache);
  faio::detail::coroutine_frame_cache_scope cache;
  EXPECT_EQ(frame_allocator_test::other_tu_current_cache(), &cache.cache());
  std::atomic<unsigned> destroyed{0};
  {
    auto task = frame_allocator_test::other_tu_value(
        std::make_unique<frame_allocator_test::tracked_payload>(destroyed));
    EXPECT_EQ(destroyed.load(), 0u);  // 未启动帧仍拥有真实 move-only 参数。
  }
  EXPECT_EQ(destroyed.load(), 1u);
  EXPECT_GT(cache.cache().retained_bytes(), 0u);
}

TEST(CoroutineFrameCacheTest, SuspendedFrameCannotBeReusedByAnotherTask) {
  faio::detail::coroutine_frame_cache_scope cache;
  std::atomic<unsigned> destroyed{0};
  std::coroutine_handle<> pending{};
  frame_allocator_test::owned_frame<int> first{frame_allocator_test::other_tu_pending_value(
      std::make_unique<frame_allocator_test::tracked_payload>(destroyed), pending)};
  first.handle.resume();  // 真正挂起，并把地址稳定的续体交给测试。
  ASSERT_EQ(pending.address(), first.handle.address());
  ASSERT_FALSE(first.handle.done());
  {
    frame_allocator_test::owned_frame<int> second{frame_allocator_test::other_tu_pending_value(
        std::make_unique<frame_allocator_test::tracked_payload>(destroyed), pending)};
    EXPECT_NE(second.handle.address(), first.handle.address());  // 在途帧不可进入 free list。
  }
  EXPECT_EQ(destroyed.load(), 1u);
  pending = {};
  first.handle.resume();
  ASSERT_TRUE(first.handle.done());
  EXPECT_EQ(first.handle.promise().take_result(), 42);
  EXPECT_EQ(destroyed.load(), 1u);  // 原参数只有 first 的拥有者 destroy 后才归还。
}

TEST(CoroutineFrameCacheTest, AllocationSurvivesOriginThreadExitAndForeignDestroy) {
  std::atomic<unsigned> destroyed{0};
  std::optional<faio::task<int>> transferred;
  std::jthread allocating([&] {
    faio::detail::coroutine_frame_cache_scope cache;
    transferred.emplace(frame_allocator_test::other_tu_value(
        std::make_unique<frame_allocator_test::tracked_payload>(destroyed)));
  });
  allocating.join();  // 原运行栈与线程已退出，header 不能借用其 cache 或 TLS 地址。
  EXPECT_EQ(destroyed.load(), 0u);
  std::atomic<std::size_t> retained{0};
  std::jthread destroying([task = std::move(*transferred), &destroyed, &retained]() mutable {
    faio::detail::coroutine_frame_cache_scope cache;
    {
      auto owner = std::move(task);  // 未启动任务的合法析构路径。
    }
    EXPECT_EQ(destroyed.load(), 1u);
    retained.store(cache.cache().retained_bytes(), std::memory_order_relaxed);
  });
  destroying.join();
  EXPECT_EQ(destroyed.load(), 1u);
  EXPECT_GT(retained.load(), 0u);  // 原 global 块可安全进入销毁线程的缓存。
}

TEST(CoroutineFrameCacheTest, LateTlsTaskDestructionUsesGlobalFallbackAfterScopeExit) {
  std::atomic<unsigned> destroyed{0};
  std::jthread thread([&] {
    (void)frame_allocator_test::late_task;  // TLS optional 本体活到线程退出。
    {
      faio::detail::coroutine_frame_cache_scope cache;
      frame_allocator_test::late_task.emplace(frame_allocator_test::other_tu_value(
          std::make_unique<frame_allocator_test::tracked_payload>(destroyed)));
    }  // 先撤去当前 cache 借用，再释放全部空闲块。
    EXPECT_EQ(faio::detail::current_frame_cache, nullptr);
    EXPECT_EQ(destroyed.load(), 0u);
  });
  thread.join();  // TLS task 现在才析构；不得访问已消失的运行栈 cache。
  EXPECT_EQ(destroyed.load(), 1u);
}

TEST(CoroutineFrameCacheTest, NestedScopesRestoreOuterCacheOnException) {
  auto* previous = faio::detail::current_frame_cache;
  {
    faio::detail::coroutine_frame_cache_scope outer;
    EXPECT_EQ(faio::detail::current_frame_cache, &outer.cache());
    try {
      faio::detail::coroutine_frame_cache_scope inner;
      EXPECT_EQ(faio::detail::current_frame_cache, &inner.cache());
      auto* storage = faio::detail::allocate_coroutine_frame(256, alignof(std::max_align_t));
      faio::detail::release_coroutine_frame(storage);
      EXPECT_GT(inner.cache().retained_bytes(), 0u);
      throw std::runtime_error("cache scope 异常退出");
    } catch (const std::runtime_error&) {
    }
    EXPECT_EQ(faio::detail::current_frame_cache, &outer.cache());
  }
  EXPECT_EQ(faio::detail::current_frame_cache, previous);
}

TEST(CoroutineFrameCacheTest, CompilerConstructorFailureAndBothDeleteFormsReleaseStorage) {
  faio::detail::coroutine_frame_cache_scope cache;
  EXPECT_THROW((void)frame_allocator_test::construct_probe(true), std::runtime_error);
  EXPECT_GT(cache.cache().retained_bytes(), 0u);  // 编译器构造失败已回收 storage。
  using promise = faio::detail::task_promise<int>;
  auto* first = promise::operator new(300);
  promise::operator delete(first, 300);  // 即使调用方有 size，唯一释放信息来自 header。
  auto* second = promise::operator new(300);
  EXPECT_EQ(second, first);
  promise::operator delete(second);  // unsized 同样必须归还正确原始块。
  auto* third = promise::operator new(300);
  EXPECT_EQ(third, first);
  promise::operator delete(third, 300);
}

/** @brief 即使公开 task promise 默认构造不抛异常，帧内参数 move 失败仍必须释放 storage。 */
TEST(CoroutineFrameCacheTest, PublicTaskParameterMoveFailureReleasesActualFrameAndCopiedPayload) {
  faio::detail::coroutine_frame_cache_scope cache;
  frame_allocator_test::parameter_construction_probe probe;
  ASSERT_EQ(cache.cache().retained_bytes(), 0u);  // 独立空缓存，没有旧块让释放断言偶然成立。
  {
    frame_allocator_test::throwing_move_param caller{probe, true};
    const auto caller_address = reinterpret_cast<std::uintptr_t>(&caller);
    EXPECT_THROW((void)frame_allocator_test::other_tu_throwing_parameter(caller),
                 std::runtime_error);
    EXPECT_EQ(probe.copies, 1u);         // lvalue → by-value caller 参数只完成一次正常 copy。
    EXPECT_EQ(probe.move_attempts, 1u);  // ramp 用 formal 参数的 xvalue 构造 frame copy 时抛异常。
    EXPECT_EQ(probe.copy_source, caller_address);
    EXPECT_EQ(probe.move_source, probe.copy_target);  // 失败来源是 caller 参数副本，不能是原对象。
    EXPECT_NE(probe.move_target, probe.copy_target);  // frame 与 caller formal 不是同一个对象。
    EXPECT_EQ(probe.completed, 2u);    // 原对象和 caller formal 完成；frame 参数未完成构造。
    EXPECT_EQ(probe.destructors, 1u);  // caller formal 恰好析构一次，失败对象没有完整析构调用。
    EXPECT_EQ(probe.payload_destroyed.load(), 1u);  // formal 的实际拥有 payload 已释放。
    EXPECT_EQ(caller.value(), 42);                  // 原 lvalue payload 仍有效，不能被失败帧偷走。
    EXPECT_GT(cache.cache().retained_bytes(),
              0u);  // 只有实际 frame 释放可进入此缓存，不能弱化为允许消除。
#if defined(FAIO_DETAIL_FRAME_ASAN)
    EXPECT_NE(__asan_address_is_poisoned(reinterpret_cast<void*>(probe.move_target)), 0);
#endif
  }
  EXPECT_EQ(probe.destructors, 2u);
  EXPECT_EQ(probe.payload_destroyed.load(), 2u);  // 原对象离开作用域后才归还第二份 payload。
}

TEST(CoroutineFrameCacheTest, CacheRetainsAtMostFixedByteBoundAndLargeBlocksBypass) {
  faio::detail::coroutine_frame_cache_scope cache;
  std::vector<void*> storage;
  for (std::size_t bin = 0; bin < faio::detail::frame_cache_bin_count; ++bin) {
    const auto bytes = faio::detail::coroutine_frame_cache::block_size(bin)
                       - faio::detail::frame_allocation_prefix;
    for (unsigned count = 0; count < 6; ++count)
      storage.push_back(faio::detail::allocate_coroutine_frame(bytes, alignof(std::max_align_t)));
  }
  for (auto* frame : storage)
    faio::detail::release_coroutine_frame(frame);
  EXPECT_LE(cache.cache().retained_bytes(), 256 * 1024u);
  EXPECT_EQ(cache.cache().retained_bytes(), faio::detail::frame_cache_max_retained);
  const auto retained = cache.cache().retained_bytes();
  auto* large = faio::detail::allocate_coroutine_frame(faio::detail::frame_cache_max_block + 1,
                                                       alignof(std::max_align_t));
  faio::detail::release_coroutine_frame(large);
  EXPECT_EQ(cache.cache().retained_bytes(), retained);
  cache.cache().clear();
  EXPECT_EQ(cache.cache().retained_bytes(), 0u);
  EXPECT_THROW((void)faio::detail::allocate_coroutine_frame(std::numeric_limits<std::size_t>::max(),
                                                            alignof(std::max_align_t)),
               std::bad_alloc);
}

TEST(CoroutineFrameCacheTest, OveralignedPromiseAndFramePayloadKeepRequestedAlignment) {
  faio::detail::coroutine_frame_cache_scope cache;
  {
    frame_allocator_test::owned_frame<frame_allocator_test::aligned_result> frame{
        frame_allocator_test::other_tu_aligned_value()};
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&frame.handle.promise()) % 128, 0u);
    frame.handle.resume();
    ASSERT_TRUE(frame.handle.done());
    EXPECT_EQ(frame.handle.promise().take_result().value, 42);
  }
  EXPECT_EQ(cache.cache().retained_bytes(), 0u);  // 完整 destroy 后过对齐块仍不进入缓存。
}

#if defined(FAIO_DETAIL_FRAME_ASAN)
TEST(CoroutineFrameCacheTest, AsanPoisonsDestroyedPayloadAndUnpoisonsReusedStorage) {
  faio::detail::coroutine_frame_cache_scope cache;
  auto* storage = faio::detail::allocate_coroutine_frame(512, alignof(std::max_align_t));
  EXPECT_EQ(__asan_address_is_poisoned(storage), 0);
  auto* bytes = static_cast<std::byte*>(storage);
  EXPECT_EQ(__asan_address_is_poisoned(bytes + 511), 0);
  EXPECT_NE(__asan_address_is_poisoned(bytes + 512), 0);  // fresh bin 尾部不是有效帧范围。
  EXPECT_NE(__asan_address_is_poisoned(bytes - 1), 0);    // 独立 header 前缀不隐藏帧下溢。
  faio::detail::release_coroutine_frame(storage);
  EXPECT_NE(__asan_address_is_poisoned(storage), 0);  // 缓存保留 storage 不得掩盖 UAF。
  auto* header = bytes - faio::detail::frame_allocation_prefix;
  EXPECT_EQ(__asan_address_is_poisoned(header), 0);  // free list 的独立元数据仍可访问。
  auto* reused = faio::detail::allocate_coroutine_frame(544, alignof(std::max_align_t));
  EXPECT_EQ(reused, storage);
  EXPECT_EQ(__asan_address_is_poisoned(reused), 0);
  auto* larger = static_cast<std::byte*>(reused);
  EXPECT_EQ(__asan_address_is_poisoned(larger + 512), 0);  // 新 requested size 精确扩张有效范围。
  EXPECT_EQ(__asan_address_is_poisoned(larger + 543), 0);
  EXPECT_NE(__asan_address_is_poisoned(larger + 544), 0);
  faio::detail::release_coroutine_frame(reused);
  auto* partial = faio::detail::allocate_coroutine_frame(513, alignof(std::max_align_t));
  EXPECT_EQ(partial, storage);
  auto* partial_bytes = static_cast<std::byte*>(partial);
  EXPECT_EQ(__asan_address_is_poisoned(partial_bytes + 512), 0);
  EXPECT_NE(__asan_address_is_poisoned(partial_bytes + 513),
            0);  // 同一shadow字节的最后1个有效字节。
  faio::detail::release_coroutine_frame(partial);
}

TEST(CoroutineFrameCacheTest, AsanKeepsOveralignedPaddingOutsideExactFrameBoundary) {
  faio::detail::coroutine_frame_cache_scope cache;
  auto* storage = faio::detail::allocate_coroutine_frame(513, 64);
  auto* bytes = static_cast<std::byte*>(storage);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(storage) % 64, 0u);
  EXPECT_EQ(__asan_address_is_poisoned(bytes + 512), 0);
  EXPECT_NE(__asan_address_is_poisoned(bytes + 513),
            0);  // 64对齐仍保持8字节shadow内的partial边界。
  EXPECT_NE(__asan_address_is_poisoned(bytes - 1), 0);
  auto* header = bytes - faio::detail::frame_allocation_prefix;
  EXPECT_EQ(__asan_address_is_poisoned(header), 0);
  EXPECT_NE(__asan_address_is_poisoned(header - 1), 0);  // header 前的对齐填充或globalredzone。
  faio::detail::release_coroutine_frame(storage);
  EXPECT_EQ(cache.cache().retained_bytes(), 0u);  // 对齐填充的 exact 块始终走 global delete。
}
#endif

namespace fast_chain_budget_test {
/** @brief 只记录真实 task 入口/完成后的 TLS；不自行实现预算递减或补充。 */
struct observations {
  std::vector<unsigned> ids;
  std::vector<std::uint16_t> entered;
  std::vector<std::uint16_t> completed;
  std::vector<std::uint16_t> child_entered;
};

/** @brief 真正惰性task帧的独占测试所有者；显式设置正式让出所需的调度归属。 */
struct task_frame {
  faio::task<void>::handle_type handle;

  explicit task_frame(faio::task<void> task) noexcept : handle(task.take()) {
    handle.promise().context.scheduler =
        faio::detail::current_scheduler();  // 不经root包装时明确绑定。
  }

  task_frame(const task_frame&) = delete;

  task_frame& operator=(const task_frame&) = delete;

  ~task_frame() {
    if (handle)
      handle.destroy();
  }  // 无内核借用；所有手工队列均已停止执行。
};

faio::task<void> consume(unsigned id, unsigned checks, observations& seen) {
  seen.ids.push_back(id);
  seen.entered.push_back(faio::detail::current_cooperative_budget);
  for (unsigned index = 0; index < checks; ++index)
    co_await faio::this_coro::yield_if_needed();  // 使用真正公开 awaiter，耗尽会重新入 FIFO。
  seen.completed.push_back(faio::detail::current_cooperative_budget);
}

faio::task<void> child_check(observations& seen) {
  seen.child_entered.push_back(faio::detail::current_cooperative_budget);
  co_await faio::this_coro::yield_if_needed();  // 父子对称转移不得取得另一份额度。
}

faio::task<void> parent_checks(observations& seen) {
  seen.ids.push_back(2);
  seen.entered.push_back(faio::detail::current_cooperative_budget);
  for (unsigned index = 0; index < 30; ++index)
    co_await child_check(seen);  // 每个子帧完整创建/对称转移/销毁，使用同一 TLS 真值。
  for (unsigned index = 0; index < 7; ++index)
    co_await faio::this_coro::yield_if_needed();
  seen.completed.push_back(faio::detail::current_cooperative_budget);
}

/** @brief 手工单线程恢复记录真实spawn子任务在父条件让出检查数中的执行位置。 */
struct self_fifo_observations {
  unsigned parent_checks{}, child_runs{}, parent_checks_at_child{};
};

faio::task<void> spawned_fast_child(self_fifo_observations& seen) {
  ++seen.child_runs;
  seen.parent_checks_at_child = seen.parent_checks;  // 只由手工所属线程读写，不假设其他OS线程运行。
  co_return;
}

/** @brief 父每64次真实检查重新进入FIFO，同时通过公开spawn留下一个等待的fast子帧。 */
faio::task<void> repeatedly_exhausting_parent(self_fifo_observations& seen) {
  auto child =
      faio::spawn(spawned_fast_child(seen));  // 当前TLS域真实投递，不能用测试直接代替fast发布。
  for (unsigned count = 0; count < 3 * faio::detail::cooperative_budget_limit; ++count) {
    ++seen.parent_checks;
    co_await faio::this_coro::yield_if_needed();  // 三次实际耗尽，不改预算或强制重置来源。
  }
  co_await child;  // 失败断言后仍能在有界剩余恢复中排空根包装与共享结果。
}

/** @brief 使用与 worker 相同的来源选择/执行作用域，真实恢复独占 task 帧。 */
void resume(faio::runtime::detail::local_scheduler& scheduler, std::coroutine_handle<> handle) {
  scheduler.before_execute();               // 搜索与通知责任保持正式实现，不用测试替代预算策略。
  auto poll = scheduler.begin_execution();  // 同 worker::execute 的生产入口。
  const auto tracker = faio::detail::current_tracker;
  auto stop = faio::detail::current_stop_token;
  faio::detail::current_tracker = nullptr;
  faio::detail::current_stop_token = {};
  handle.resume();  // 真实协程协议包括 yield、子任务和 final_suspend。
  faio::detail::current_tracker = tracker;
  faio::detail::current_stop_token = std::move(stop);
}

/** @brief 每个测试单独注册两 worker，但不启线程；队列/句柄执行顺序完全由测试控制。 */
struct environment {
  faio::runtime::detail::domain_scheduler domain{2};
  scheduler_sleep_batch_test::counting_waker first_waker, second_waker;
  faio::runtime::detail::local_scheduler first{
      domain, 0, 61, faio::runtime::detail::worker_waker_ref{first_waker}};
  faio::runtime::detail::local_scheduler second{
      domain, 1, 61, faio::runtime::detail::worker_waker_ref{second_waker}};
  faio::detail::execution_thread_binding binding{
      faio::scheduler_ref{domain}, {}, first.local_state(), 0};
  faio::detail::execution_thread_guard guard{binding};  // domain.enqueue_io 精确进入 first 私有槽。

  std::optional<std::coroutine_handle<>> run(unsigned tick = 1) {
    auto selected = first.next_task(tick);  // 选择失败不恢复，也不伪造预算消耗。
    if (selected)
      resume(first, *selected);
    return selected;
  }
};

faio::task<std::array<std::uint16_t, 2>> pipe_budget_after_real_io(int fd) {
  for (unsigned index = 0; index < 63; ++index)
    co_await faio::this_coro::yield_if_needed();
  const auto before = faio::detail::current_cooperative_budget;
  char value{};
  const auto result = co_await faio::io::read(fd, &value, 1, 0);  // 无数据时真正挂起到 IO driver。
  if (!result || *result != 1 || value != 'x')
    throw std::runtime_error("真实 IO 恢复预算测试的 pipe payload 错误");
  co_return std::array<std::uint16_t, 2>{before, faio::detail::current_cooperative_budget};
}

faio::task<void> delayed_pipe_write(int fd) {
  co_await faio::time::sleep(std::chrono::milliseconds(10));  // 单 worker 下 reader 必先完成挂起。
  const char value = 'x';
  const auto result = co_await faio::io::write(fd, &value, 1, 0);
  if (!result || *result != 1)
    throw std::runtime_error("真实 IO 恢复预算测试的 pipe 写失败");
}
}  // namespace fast_chain_budget_test

TEST(FastChainBudgetContract, DifferentPrivateFastTasksInheritRemainingBudget) {
  fast_chain_budget_test::observations seen;
  fast_chain_budget_test::environment environment;
  fast_chain_budget_test::task_frame initial{fast_chain_budget_test::consume(1, 20, seen)};
  fast_chain_budget_test::task_frame first{fast_chain_budget_test::consume(2, 12, seen)};
  fast_chain_budget_test::task_frame second{fast_chain_budget_test::consume(3, 7, seen)};
  environment.first.local_state().push_back(initial.handle, environment.domain.global_queue());
  ASSERT_TRUE(environment.run());
  environment.domain.enqueue_io(first.handle);
  ASSERT_TRUE(environment.run());
  environment.domain.enqueue_io(second.handle);
  ASSERT_TRUE(environment.run());
  EXPECT_EQ(seen.ids, (std::vector<unsigned>{1, 2, 3}));
  EXPECT_EQ(seen.entered, (std::vector<std::uint16_t>{64, 44, 32}));
  EXPECT_EQ(seen.completed, (std::vector<std::uint16_t>{44, 32, 25}));
}

TEST(FastChainBudgetContract, FifoGlobalAndStolenSelectionsStartFreshEpoch) {
  fast_chain_budget_test::observations seen;
  fast_chain_budget_test::environment environment;
  fast_chain_budget_test::task_frame initial{fast_chain_budget_test::consume(1, 60, seen)};
  fast_chain_budget_test::task_frame fast{fast_chain_budget_test::consume(2, 1, seen)};
  fast_chain_budget_test::task_frame fifo{fast_chain_budget_test::consume(3, 1, seen)};
  fast_chain_budget_test::task_frame global{fast_chain_budget_test::consume(4, 1, seen)};
  fast_chain_budget_test::task_frame stolen{fast_chain_budget_test::consume(5, 1, seen)};
  environment.first.local_state().push_back(initial.handle, environment.domain.global_queue());
  ASSERT_TRUE(environment.run());
  environment.first.local_state().push_back(fifo.handle, environment.domain.global_queue());
  environment.domain.enqueue_io(fast.handle);
  ASSERT_TRUE(environment.run());  // 私有 fast 得到4，而不是64。
  ASSERT_TRUE(environment.run());  // 下一条 FIFO 与 fast 不是同一个执行 epoch。
  environment.domain.global_queue().push_back(global.handle);
  ASSERT_TRUE(environment.run(61));  // 实际全局公平选择必须新建预算。
  environment.second.local_state().push_back(stolen.handle, environment.domain.global_queue());
  const auto selected = environment.first.steal_task();
  ASSERT_TRUE(selected);
  ASSERT_EQ(selected->address(), stolen.handle.address());
  fast_chain_budget_test::resume(environment.first, *selected);
  EXPECT_EQ(seen.entered, (std::vector<std::uint16_t>{64, 4, 64, 64, 64}));
  EXPECT_EQ(seen.completed, (std::vector<std::uint16_t>{4, 3, 63, 63, 63}));
}

TEST(FastChainBudgetContract, ExhaustedFastChainRunsWaitingFifoBeforeNewFastTask) {
  fast_chain_budget_test::observations seen;
  fast_chain_budget_test::environment environment;
  fast_chain_budget_test::task_frame initial{fast_chain_budget_test::consume(1, 63, seen)};
  fast_chain_budget_test::task_frame exhausting{fast_chain_budget_test::consume(2, 1, seen)};
  fast_chain_budget_test::task_frame waiting{fast_chain_budget_test::consume(3, 0, seen)};
  fast_chain_budget_test::task_frame fresh_fast{fast_chain_budget_test::consume(4, 0, seen)};
  environment.first.local_state().push_back(initial.handle, environment.domain.global_queue());
  ASSERT_TRUE(environment.run());
  environment.first.local_state().push_back(waiting.handle, environment.domain.global_queue());
  environment.domain.enqueue_io(exhausting.handle);
  ASSERT_TRUE(environment.run());  // 最后1额度真正耗尽，exhausting由公开awaiter进入FIFO。
  EXPECT_FALSE(exhausting.handle.done());
  environment.domain.enqueue_io(fresh_fast.handle);
  const auto selected = environment.run();
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->address(),
            waiting.handle.address());  // 严格下一次 FIFO，不被新增 fast 插队。
  ASSERT_TRUE(environment.run());
  ASSERT_TRUE(environment.run());  // exhausting 从 FIFO 真实恢复，恰好完成。
  EXPECT_TRUE(exhausting.handle.done());
  EXPECT_EQ(seen.ids, (std::vector<unsigned>{1, 2, 3, 4}));
  EXPECT_EQ(seen.entered, (std::vector<std::uint16_t>{64, 1, 64, 64}));
  EXPECT_EQ(seen.completed, (std::vector<std::uint16_t>{1, 64, 64, 64}));
}

TEST(FastChainBudgetContract, ExhaustedOnlyFastFallbackStillMakesProgressAfterPeerStealsFifo) {
  fast_chain_budget_test::observations seen;
  fast_chain_budget_test::environment environment;
  fast_chain_budget_test::task_frame initial{fast_chain_budget_test::consume(1, 63, seen)};
  fast_chain_budget_test::task_frame exhausting{fast_chain_budget_test::consume(2, 1, seen)};
  fast_chain_budget_test::task_frame only_fast{fast_chain_budget_test::consume(3, 1, seen)};
  environment.first.local_state().push_back(initial.handle, environment.domain.global_queue());
  ASSERT_TRUE(environment.run());
  environment.domain.enqueue_io(exhausting.handle);
  ASSERT_TRUE(environment.run());  // exhausting实际重新入FIFO，第一worker剩余为0。
  const auto peer_selected = environment.second.steal_task();  // 真实窃取把唯一FIFO项移走。
  ASSERT_TRUE(peer_selected);
  ASSERT_EQ(peer_selected->address(), exhausting.handle.address());
  environment.domain.enqueue_io(only_fast.handle);
  ASSERT_TRUE(environment.run());  // FIFO被偷空也不能遗留永不恢复的私有fast。
  EXPECT_TRUE(only_fast.handle.done());
  faio::detail::execution_thread_binding peer_binding{
      faio::scheduler_ref{environment.domain}, {}, environment.second.local_state(), 1};
  {
    faio::detail::execution_thread_guard peer_guard{peer_binding};
    fast_chain_budget_test::resume(environment.second, *peer_selected);
  }
  EXPECT_TRUE(exhausting.handle.done());
  EXPECT_EQ(seen.entered, (std::vector<std::uint16_t>{64, 1, 64}));
  EXPECT_EQ(seen.completed, (std::vector<std::uint16_t>{1, 63, 64}));
}

/** @brief 常驻selfFIFO每64次耗尽时，等待fast子任务必须按降级后的有限FIFO顺序执行。 */
TEST(FastChainBudgetContract, RepeatedSelfFifoExhaustionCannotStarveSpawnedFastChild) {
  fast_chain_budget_test::self_fifo_observations seen;
  fast_chain_budget_test::environment environment;
  fast_chain_budget_test::task_frame parent{
      fast_chain_budget_test::repeatedly_exhausting_parent(seen)};
  environment.first.local_state().push_back(parent.handle, environment.domain.global_queue());
  EXPECT_TRUE(environment.run());  // 父首次恢复创建真实fast子根，然后在64检查处自让出。
  EXPECT_EQ(seen.parent_checks, 64u);
  EXPECT_EQ(seen.child_runs, 0u);
  EXPECT_TRUE(environment.run());  // 当前selfFIFO先恢复；原fast子任务已降级到其后面。
  EXPECT_EQ(seen.parent_checks, 128u);
  EXPECT_EQ(seen.child_runs, 0u);
  EXPECT_TRUE(environment.run());  // 严格第三次选择子任务；不能依赖IO/timer或OS时间片救援。
  EXPECT_EQ(seen.child_runs, 1u);
  EXPECT_EQ(seen.parent_checks_at_child, 128u);
  // 余下最多父第三轮、自让出恢复、join恢复三步；旧实现失败也完成清理而不泄漏子根。
  for (unsigned remaining = 0; remaining < 3 && !parent.handle.done(); ++remaining)
    EXPECT_TRUE(environment.run());
  EXPECT_TRUE(parent.handle.done());
  EXPECT_EQ(seen.parent_checks, 192u);
  EXPECT_EQ(seen.child_runs, 1u);
  EXPECT_FALSE(environment.first.has_ready_task());
  EXPECT_TRUE(environment.domain.global_queue().empty());
}

TEST(FastChainBudgetContract, DriverBoundaryAndNoReadyResultDiscardOldFastQuota) {
  fast_chain_budget_test::observations seen;
  fast_chain_budget_test::environment environment;
  fast_chain_budget_test::task_frame initial{fast_chain_budget_test::consume(1, 63, seen)};
  fast_chain_budget_test::task_frame after_driver{fast_chain_budget_test::consume(2, 63, seen)};
  fast_chain_budget_test::task_frame after_empty{fast_chain_budget_test::consume(3, 1, seen)};
  environment.first.local_state().push_back(initial.handle, environment.domain.global_queue());
  ASSERT_TRUE(environment.run());
  environment.domain.enqueue_io(after_driver.handle);
  const auto selected_before_drive =
      environment.first.next_task(1);  // 与时间预算drive一样先选择fast。
  ASSERT_TRUE(selected_before_drive);
  ASSERT_EQ(selected_before_drive->address(), after_driver.handle.address());
  environment.first.interrupt_execution_chain();  // driver必须让已经选中但未resume的任务也fresh。
  fast_chain_budget_test::resume(environment.first, *selected_before_drive);
  EXPECT_FALSE(environment.first.next_task(61));  // 全局周期的真实nullopt路径同样结束旧链。
  environment.domain.enqueue_io(after_empty.handle);
  ASSERT_TRUE(environment.run());
  EXPECT_EQ(seen.entered, (std::vector<std::uint16_t>{64, 64, 64}));
  EXPECT_EQ(seen.completed, (std::vector<std::uint16_t>{1, 1, 63}));
}

TEST(FastChainBudgetContract, ParentChildSymmetricTransferUsesInheritedFastQuota) {
  fast_chain_budget_test::observations seen;
  fast_chain_budget_test::environment environment;
  fast_chain_budget_test::task_frame initial{fast_chain_budget_test::consume(1, 24, seen)};
  fast_chain_budget_test::task_frame parent{fast_chain_budget_test::parent_checks(seen)};
  environment.first.local_state().push_back(initial.handle, environment.domain.global_queue());
  ASSERT_TRUE(environment.run());
  environment.domain.enqueue_io(parent.handle);
  ASSERT_TRUE(environment.run());
  EXPECT_TRUE(parent.handle.done());
  EXPECT_EQ(seen.entered, (std::vector<std::uint16_t>{64, 40}));
  EXPECT_EQ(seen.completed, (std::vector<std::uint16_t>{40, 3}));
  ASSERT_EQ(seen.child_entered.size(), 30u);
  EXPECT_EQ(seen.child_entered.front(), 40u);
  EXPECT_EQ(seen.child_entered.back(), 11u);  // 每次真实子task只消耗1，context镜像未重置TLS。
}

TEST(FastChainBudgetContract, ExecutionScopeRestoresOuterTlsAfterNestedAndExceptionalScopes) {
  fast_chain_budget_test::observations seen;
  fast_chain_budget_test::environment environment;
  fast_chain_budget_test::task_frame initial{fast_chain_budget_test::consume(1, 20, seen)};
  fast_chain_budget_test::task_frame after_exception{fast_chain_budget_test::consume(2, 1, seen)};
  const auto outside = faio::detail::current_cooperative_budget;
  {
    faio::detail::cooperative_poll_scope outer{7};  // 外层不是本worker链的拥有者。
    environment.first.local_state().push_back(initial.handle, environment.domain.global_queue());
    ASSERT_TRUE(environment.run());
    EXPECT_EQ(faio::detail::current_cooperative_budget, 7u);
    try {
      auto scope = environment.first.begin_execution();
      faio::detail::current_cooperative_budget = 5;  // 测试scope异常还原，不冒充业务预算递减。
      throw std::runtime_error("scope展开");
    } catch (const std::runtime_error&) {
    }
    EXPECT_EQ(faio::detail::current_cooperative_budget, 7u);
    environment.domain.enqueue_io(after_exception.handle);
    ASSERT_TRUE(environment.run());  // 异常scope捕获的是本链5，不能误取已还原外层7。
    EXPECT_EQ(seen.entered, (std::vector<std::uint16_t>{64, 5}));
    EXPECT_EQ(seen.completed, (std::vector<std::uint16_t>{44, 4}));
    EXPECT_EQ(faio::detail::current_cooperative_budget, 7u);
  }
  EXPECT_EQ(faio::detail::current_cooperative_budget, outside);
}

TEST(FastChainBudgetContract, RealSuspendedPipeIoGetsFreshQuotaOnFastResume) {
  scheduler_test::pipe_descriptors pipe;
  faio::runtime::detail::runtime_context runtime{
      faio_test::config_builder().set_num_workers(1).build()};
  const auto result = runtime.block_on(
      faio::join(fast_chain_budget_test::pipe_budget_after_real_io(pipe.descriptors[0]),
                 fast_chain_budget_test::delayed_pipe_write(pipe.descriptors[1])));
  EXPECT_EQ(std::get<0>(result), (std::array<std::uint16_t, 2>{1, 64}));
}
