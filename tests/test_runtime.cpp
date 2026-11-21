/**
 * @file test_runtime.cpp
 * @brief 任务结果、并发提交、取消与运行时模式的单元测试。
 */
#include "test_helpers.hpp"
#include <faio/detail/blocking/service.hpp>
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <future>
#include <latch>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <tuple>
#include <vector>

namespace {
using runtime = faio::runtime::detail::runtime_context;
using namespace std::chrono_literals;

/** @brief 返回给定值，可用于任务组合。 */
faio::task<int> value_task(int value) {
  co_return value;
}

/** @brief 返回独占结果，检查组合器是否通过移动交付结果。 */
faio::task<std::unique_ptr<int>> unique_value_task(int value) {
  co_return std::make_unique<int>(value);
}

/** @brief 在帧内持有资源，只有实际执行时才增加次数。 */
faio::task<int> owned_join_value(std::shared_ptr<int> value, int& executions) {
  ++executions;
  co_return *value;
}

/** @brief 传播协程执行期间的异常。 */
faio::task<int> failed_task() {
  throw std::runtime_error("测试异常");
  co_return 0;
}

/** @brief 让出执行权后更新共享计数。 */
faio::task<void> increment(std::atomic<int>& count) {
  co_await faio::this_coro::yield();
  count.fetch_add(1);
}

/** @brief 提交后台子任务，由根任务等待它完成。 */
faio::task<void> spawn_children(std::atomic<int>& count) {
  faio::spawn_detached(increment(count));
  faio::spawn_detached(increment(count));
  co_return;
}

/** @brief 协程挂起等待取消。 */
faio::task<void> pending_task() {
  co_await faio::time::sleep(1h);
}

/** @brief 请求取消，并领取子任务的取消结果。 */
faio::task<bool> cancel_child() {
  auto child = faio::spawn(pending_task());
  co_await faio::this_coro::yield();
  child.request_stop();
  try {
    co_await child;
  } catch (const faio::operation_cancelled&) {
    co_return true;
  }
  co_return false;
}

/** @brief 等待独立线程执行同步任务；任务通过移动捕获数据。 */
faio::task<int> blocking_value() {
  const auto caller = std::this_thread::get_id();
  auto handle = faio::spawn_blocking([value = std::make_unique<int>(42), caller] {
    if (caller == std::this_thread::get_id())
      throw std::logic_error("阻塞工作未隔离");
    return *value;
  });
  co_return co_await handle;
}

/** @brief 读取当前运行时的用户阻塞线程数。 */
faio::task<std::size_t> blocking_worker_count() {
  co_return faio::detail::current_blocking_pool()->service().started_threads();
}

/** @brief 等待短定时器并返回。 */
faio::task<void> sleep_briefly() {
  co_await faio::time::sleep(5ms);
}

/** @brief 记录外部提交的任务实际在哪个线程运行，再唤醒等待者。 */
faio::task<void> record_driver(faio::sync::semaphore& completed, std::thread::id& driver) {
  driver = std::this_thread::get_id();
  completed.release();
  co_return;
}

/** @brief 挂起当前驱动，等待外部线程投递的任务运行。 */
faio::task<std::thread::id> await_external_work(std::latch& entered,
                                                faio::sync::semaphore& completed) {
  entered.count_down();
  co_await completed.acquire();
  co_return std::this_thread::get_id();
}

/** @brief 在所属驱动上记录一次外部投递，并递减等待计数。 */
faio::task<void> mark_external_task(int& count, faio::sync::latch& completed) {
  ++count;
  completed.count_down();
  co_return;
}

/** @brief 记录一个 join 分支的执行次数，并返回输入位置。 */
faio::task<std::size_t> record_join_child(std::size_t index, int& count) {
  ++count;
  co_return index;
}

/** @brief 执行一组分支，检查各分支的执行次数与结果顺序。 */
faio::task<std::vector<std::size_t>> join_recorded_children(std::span<int> counts) {
  std::vector<faio::task<std::size_t>> children;
  children.reserve(counts.size());
  for (std::size_t i = 0; i < counts.size(); ++i)
    children.push_back(record_join_child(i, counts[i]));
  co_return co_await faio::join_all(std::move(children));
}

/** @brief 在 block_on 等待外层任务结束之前，检查 join 何时交付异常。 */
faio::task<bool> join_drains_failing_group() {
  std::atomic<int> completed{};
  try {
    (void)co_await faio::join(failed_task(), increment(completed), increment(completed));
  } catch (const std::runtime_error&) {
    co_return completed.load() == 2;
  }
  co_return false;
}

/** @brief 失败分支立即退出，其他分支让出执行权后记录完成。 */
faio::task<std::size_t> delayed_join_child(std::size_t index, int& count, bool fail) {
  if (fail) {
    ++count;
    throw std::runtime_error("批量分支失败");
  }
  co_await faio::this_coro::yield();
  ++count;
  co_return index;
}

/** @brief 在 block_on 排空根任务前，检查跨批次的失败是否等待全部分支退出。 */
faio::task<bool> join_drains_failing_batch(std::span<int> counts) {
  std::vector<faio::task<std::size_t>> children;
  children.reserve(counts.size());
  for (std::size_t i = 0; i < counts.size(); ++i)
    children.push_back(delayed_join_child(i, counts[i], i == counts.size() - 1));
  try {
    (void)co_await faio::join_all(std::move(children));
  } catch (const std::runtime_error&) {
    for (const auto count : counts)
      if (count != 1)
        co_return false;
    co_return true;
  }
  co_return false;
}

/** @brief 在准备阶段注入失败，检查未执行的任务帧已释放、完成责任已撤销。 */
faio::task<bool> rejected_join_preparation_releases_resources() {
  faio::detail::join_completion<faio::default_scheduler> completion;
  faio::detail::join_slot<int> result;
  auto* scope = faio::detail::current_task_context().scope;
  const auto initial_pending = scope->pending.load(std::memory_order_acquire);
  auto value = std::make_shared<int>(42);
  const std::weak_ptr<int> observed = value;
  int executions = 0;
  bool rejected = false;
  try {
    auto prepared = faio::detail::prepare_join_task(
        owned_join_value(std::move(value), executions), result, completion, faio::scheduler_ref{});
  } catch (const std::logic_error&) {
    rejected = true;
  }
  completion.finish_scheduling();
  co_return rejected&& observed.expired() && executions == 0
      && scope->pending.load(std::memory_order_acquire)
             == initial_pending&& completion.wait({}).await_ready();
}

/** @brief 移动准备结果后，源对象不再拥有帧，目标析构只归还一次完成责任。 */
faio::task<bool> moved_join_preparation_releases_resources() {
  faio::detail::join_completion<faio::default_scheduler> completion;
  faio::detail::join_slot<int> result;
  auto* scope = faio::detail::current_task_context().scope;
  const auto initial_pending = scope->pending.load(std::memory_order_acquire);
  auto value = std::make_shared<int>(42);
  const std::weak_ptr<int> observed = value;
  int executions = 0;
  std::optional<faio::detail::prepared_join_task<faio::default_scheduler>> pending;
  {
    auto prepared = faio::detail::prepare_join_task(
        owned_join_value(std::move(value), executions),
        result,
        completion,
        faio::detail::current_scheduler_for<faio::default_scheduler>());
    pending.emplace(std::move(prepared));
  }
  const auto retained =
      !observed.expired() && scope->pending.load(std::memory_order_acquire) == initial_pending + 1;
  pending.reset();
  completion.finish_scheduling();
  co_return retained&& observed.expired() && executions == 0
      && scope->pending.load(std::memory_order_acquire)
             == initial_pending&& completion.wait({}).await_ready();
}

/** @brief 由外层 scope 等待 join 分支提交的后台工作结束。 */
faio::task<void> spawn_scoped_join_child(faio::scope_context& scope, std::atomic<int>& count) {
  scope.spawn(increment(count));
  co_return;
}

/** @brief 检查 block_on 等待剩余工作之前，scope 管理的工作是否已经全部结束。 */
faio::task<int> scoped_join_count() {
  std::atomic<int> completed{};
  co_await faio::scope([&completed](faio::scope_context& scope) -> faio::task<void> {
    co_await faio::join(spawn_scoped_join_child(scope, completed),
                        spawn_scoped_join_child(scope, completed));
  });
  co_return completed.load();
}
}  // namespace

/** @brief 任务返回值与异常均由等待者领取。 */
TEST(Runtime, ResultsAndExceptions) {
  runtime context{faio_test::config()};
  EXPECT_EQ(context.block_on(value_task(42)), 42);
  EXPECT_THROW(context.block_on(failed_task()), std::runtime_error);
}

/** @brief 单线程与多线程运行时均支持任务组合。 */
TEST(Runtime, ModesAndJoin) {
  for (const auto mode : {faio::runtime::mode::current_thread, faio::runtime::mode::multi_thread}) {
    runtime context{faio_test::config(mode)};
    auto values = context.wait_all(value_task(17), value_task(25));
    EXPECT_EQ(std::get<0>(values) + std::get<1>(values), 42);
  }
}

/** @brief 空输入、批量阈值和满批边界均保持执行次数与输入结果顺序。 */
TEST(Runtime, JoinAllKeepsResultsAcrossSubmissionBoundaries) {
  for (const auto mode : {faio::runtime::mode::current_thread, faio::runtime::mode::multi_thread}) {
    SCOPED_TRACE(mode == faio::runtime::mode::current_thread ? "current_thread" : "multi_thread");
    runtime context{faio::ConfigBuilder{}.set_mode(mode).set_num_workers(1).build()};
    for (const std::size_t count : {0u, 1u, 7u, 8u, 31u, 32u, 33u, 64u, 65u}) {
      SCOPED_TRACE(count);
      std::vector<int> runs(count);
      const auto results = context.block_on(join_recorded_children(runs));
      ASSERT_EQ(results.size(), runs.size());
      for (std::size_t i = 0; i < runs.size(); ++i) {
        EXPECT_EQ(runs[i], 1);
        EXPECT_EQ(results[i], i);
      }
    }
  }
}

/** @brief 异构输入走批量路径时仍保留 void、重复类型和独占结果。 */
TEST(Runtime, BatchedJoinKeepsHeterogeneousResults) {
  for (const auto mode : {faio::runtime::mode::current_thread, faio::runtime::mode::multi_thread}) {
    runtime context{faio::ConfigBuilder{}.set_mode(mode).set_num_workers(1).build()};
    std::atomic<int> completed{};
    auto results = context.block_on(faio::join(value_task(0),
                                               unique_value_task(11),
                                               increment(completed),
                                               value_task(33),
                                               increment(completed),
                                               unique_value_task(55),
                                               value_task(66),
                                               increment(completed),
                                               value_task(88)));
    EXPECT_EQ(completed.load(), 3);
    EXPECT_EQ(std::get<0>(results), 0);
    ASSERT_TRUE(std::get<1>(results));
    EXPECT_EQ(*std::get<1>(results), 11);
    EXPECT_EQ(std::get<3>(results), 33);
    ASSERT_TRUE(std::get<5>(results));
    EXPECT_EQ(*std::get<5>(results), 55);
    EXPECT_EQ(std::get<6>(results), 66);
    EXPECT_EQ(std::get<8>(results), 88);
  }
}

/** @brief 单个 worker 在队列满载时仍保持分支计数和结果的输入顺序。 */
TEST(Runtime, JoinAllDrainsBeyondLocalCapacity) {
  for (const auto mode : {faio::runtime::mode::current_thread, faio::runtime::mode::multi_thread}) {
    runtime context{faio::ConfigBuilder{}.set_mode(mode).set_num_workers(1).build()};
    constexpr auto count = faio::runtime::detail::LOCAL_QUEUE_CAPACITY + 65;
    std::array<int, count> runs{};
    const auto results = context.block_on(join_recorded_children(runs));
    ASSERT_EQ(results.size(), runs.size());
    for (std::size_t i = 0; i < runs.size(); ++i) {
      EXPECT_EQ(runs[i], 1);
      EXPECT_EQ(results[i], i);
    }
  }
}

/** @brief 投递过程中私有队列满载时，join 仍等待已接纳的分支退出，并归还恢复槽。 */
TEST(Runtime, JoinAllDrainsAfterPartialSubmission) {
  constexpr std::size_t queue_capacity = 4;
  runtime context{faio::ConfigBuilder{}
                      .set_num_workers(1)
                      .set_no_stealing_queue_capacity(queue_capacity)
                      .build()};
  std::array<int, queue_capacity + 2> first_runs{};
  EXPECT_THROW(context.block_on(join_recorded_children(first_runs).with_no_stealing()),
               std::overflow_error);
  for (std::size_t i = 0; i < first_runs.size(); ++i)
    EXPECT_EQ(first_runs[i], i < queue_capacity - 1 ? 1 : 0);

  std::array<int, queue_capacity - 1> next_runs{};
  const auto results = context.block_on(join_recorded_children(next_runs).with_no_stealing());
  EXPECT_EQ(results, (std::vector<std::size_t>{0, 1, 2}));
  for (const auto count : next_runs)
    EXPECT_EQ(count, 1);
}

/** @brief 分支异常仅在兄弟分支全部退出后由 join 交付。 */
TEST(Runtime, JoinDrainsBeforePropagatingFailure) {
  runtime context{faio_test::config()};
  EXPECT_TRUE(context.block_on(join_drains_failing_group()));
}

/** @brief 最后一批分支失败时，异常只在前面各批的分支全部完成后交付。 */
TEST(Runtime, BatchedJoinAllDrainsBeforePropagatingFailure) {
  for (const auto mode : {faio::runtime::mode::current_thread, faio::runtime::mode::multi_thread}) {
    runtime context{faio::ConfigBuilder{}.set_mode(mode).set_num_workers(1).build()};
    std::array<int, 33> runs{};
    EXPECT_TRUE(context.block_on(join_drains_failing_batch(runs)));
    EXPECT_EQ(context.block_on(value_task(42)), 42);
  }
}

/** @brief 准备失败销毁尚未执行的子任务，归还 scope 登记后运行时仍可复用。 */
TEST(Runtime, JoinPreparationFailureReleasesChildAndScope) {
  runtime context{faio_test::config()};
  EXPECT_TRUE(context.block_on(rejected_join_preparation_releases_resources()));
  EXPECT_EQ(context.block_on(value_task(42)), 42);
}

/** @brief 丢弃移动后的未提交分支不会泄漏资源，也不会重复归还计数。 */
TEST(Runtime, MovedJoinPreparationReleasesUnsubmittedChildOnce) {
  runtime context{faio_test::config()};
  EXPECT_TRUE(context.block_on(moved_join_preparation_releases_resources()));
  EXPECT_EQ(context.block_on(value_task(42)), 42);
}

/** @brief 外层 scope 等待 join 分支提交的后台工作。 */
TEST(Runtime, ScopedJoinDrainsDescendants) {
  runtime context{faio_test::config()};
  EXPECT_EQ(context.block_on(scoped_join_count()), 2);
}

/** @brief 外部提交可唤醒当前驱动，同一运行时可由另一线程继续驱动。 */
TEST(Runtime, CurrentThreadExternalWakeAndHandoff) {
  auto config = faio_test::config(faio::runtime::mode::current_thread);
  runtime context{config};
  faio::sync::semaphore completed{0};
  std::latch entered{1};
  std::thread::id submitted_driver;
  std::latch first_drive_done{1};
  std::thread::id next_driver;
  std::jthread producer([&] {
    entered.wait();
    context.submit(record_driver(completed, submitted_driver));
    first_drive_done.wait();
    next_driver = context.block_on([]() -> faio::task<std::thread::id> {
      co_await faio::this_coro::yield();
      co_return std::this_thread::get_id();
    }());
  });
  const auto producer_id = producer.get_id();
  const auto first_driver = context.block_on(await_external_work(entered, completed));
  first_drive_done.count_down();
  producer.join();

  EXPECT_EQ(first_driver, std::this_thread::get_id());
  EXPECT_EQ(submitted_driver, first_driver);
  EXPECT_EQ(next_driver, producer_id);
}

/** @brief 分批接收外部普通任务，并保持定向就绪任务的交付。 */
TEST(Runtime, CurrentThreadOwnerSchedulerImportsExternalBatch) {
  auto config = faio_test::config(faio::runtime::mode::current_thread);
  runtime context{config};
  std::array<int, 300> counts{};
  faio::sync::latch completed{static_cast<std::ptrdiff_t>(counts.size())};
  std::jthread producer([&] {
    context.submit(mark_external_task(counts.front(), completed).with_no_stealing());
    for (auto& count : std::span{counts}.subspan(1))
      context.submit(mark_external_task(count, completed));
  });
  producer.join();
  context.block_on([&]() -> faio::task<void> {
    co_await completed.wait();
  }());
  for (const auto count : counts)
    EXPECT_EQ(count, 1);
}

/** @brief 根任务结束前，其后台子任务已经完成。 */
TEST(Runtime, DrainsDetachedChildren) {
  runtime context{faio_test::config()};
  std::atomic<int> count{};
  context.block_on(spawn_children(count));
  EXPECT_EQ(count.load(), 2);
}

/** @brief 阻塞任务支持移动捕获与异常传播。 */
TEST(Runtime, BlockingWork) {
  runtime context{faio_test::config()};
  EXPECT_EQ(context.block_on(blocking_value()), 42);
  auto failed = context.submit_blocking([]() -> int { throw std::runtime_error("测试异常"); });
  EXPECT_THROW(failed.get(), std::runtime_error);
}

/** @brief 两种驱动模式默认使用弹性池，显式选择固定池时预热线程。 */
TEST(Runtime, BlockingPoolPolicy) {
  for (const auto mode : {faio::runtime::mode::current_thread, faio::runtime::mode::multi_thread}) {
    auto builder = faio::ConfigBuilder{};
    builder.set_mode(mode).set_num_workers(2).set_max_blocking_threads(2);
    {
      runtime context{builder.build()};
      EXPECT_EQ(context.block_on(blocking_worker_count()), 0u);
      EXPECT_EQ(context.block_on(blocking_value()), 42);
      EXPECT_EQ(context.block_on(blocking_worker_count()), 1u);
    }
    builder.set_blocking_pool_policy(faio::runtime::blocking_pool_policy::fixed);
    runtime context{builder.build()};
    EXPECT_EQ(context.block_on(blocking_worker_count()), 2u);
  }
}

/** @brief 验证按需扩容、等待任务数限制，以及关闭时的队列排空。 */
TEST(BlockingService, ElasticCapacityAndClose) {
  std::array<std::promise<void>, 2> started;
  std::atomic<int> completed{};
  std::atomic<bool> closed{};
  faio::blocking::service service{2, 1, 10s};
  std::jthread closer;
  std::promise<void> release;
  const auto gate = release.get_future().share();

  EXPECT_EQ(service.started_threads(), 0u);
  for (std::size_t i = 0; i < started.size(); ++i) {
    auto& entered = started[i];
    auto began = entered.get_future();
    ASSERT_TRUE(service.try_submit([gate, &entered] {
      entered.set_value();
      gate.wait();
    }));
    ASSERT_EQ(began.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(service.started_threads(), i + 1);
  }

  ASSERT_TRUE(service.try_submit([&completed] { ++completed; }));
  EXPECT_EQ(service.queued(), 1u);
  auto full = service.try_submit([] {});
  ASSERT_FALSE(full);
  EXPECT_EQ(full.error().value(), EAGAIN);

  closer = std::jthread([&] {
    service.close();
    closed.store(true);
  });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  auto rejected = service.try_submit([] {});
  while (!rejected && rejected.error().value() == EAGAIN
         && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
    rejected = service.try_submit([] {});
  }
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().value(), ECANCELED);
  EXPECT_FALSE(closed.load());
  release.set_value();
  closer.join();
  EXPECT_TRUE(closed.load());
  EXPECT_EQ(completed.load(), 1);
  EXPECT_EQ(service.started_threads(), 0u);
}

/** @brief 空闲线程先被复用，保活超时后回收，下次提交重新启动。 */
TEST(BlockingService, ElasticReuseRetirementAndRestart) {
  std::atomic<int> generations{};
  faio::blocking::service service{1, 1, 250ms};
  const auto submit = [&] {
    auto generation = std::make_shared<std::promise<int>>();
    auto observed = generation->get_future();
    faio_test::take(service.try_submit([generation, &generations] {
      thread_local const int worker_generation = ++generations;
      generation->set_value(worker_generation);
    }));
    return observed;
  };

  auto initial = submit();
  ASSERT_EQ(initial.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(initial.get(), 1);
  auto warm = submit();
  ASSERT_EQ(warm.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(warm.get(), 1);

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (service.started_threads() != 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  ASSERT_EQ(service.started_threads(), 0u);

  auto restarted = submit();
  ASSERT_EQ(restarted.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(restarted.get(), 2);
}

/** @brief 普通线程通过 wait 和 get 可以观察到完整发布的结果。 */
TEST(Runtime, BlockingWaitPublishesResults) {
  runtime context{faio_test::config()};
  for (int value = 0; value != 8; ++value) {
    auto handle = context.submit_blocking([value] { return value; });
    int observed = -1;
    std::jthread waiter([&] {
      handle.wait();
      observed = handle.get();
    });
    waiter.join();
    EXPECT_EQ(observed, value);
  }
}

/** @brief 请求停止会恢复挂起任务并传播取消。 */
TEST(Runtime, Cancellation) {
  runtime context{faio_test::config()};
  EXPECT_TRUE(context.block_on(cancel_child()));
}

/** @brief 定时等待不会提前完成。 */
TEST(Runtime, Timer) {
  runtime context{faio_test::config()};
  const auto started = std::chrono::steady_clock::now();
  context.block_on(sleep_briefly());
  EXPECT_GE(std::chrono::steady_clock::now() - started, 4ms);
}
