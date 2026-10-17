#include "faio/detail/runtime/common/blocking_task.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <chrono>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace {
using namespace std::chrono_literals;

struct counted_job {
  std::atomic<int>* moves;
  std::atomic<int>* calls;

  counted_job(std::atomic<int>& move_count, std::atomic<int>& call_count)
      : moves(&move_count), calls(&call_count) {}

  counted_job(const counted_job&) = delete;

  counted_job(counted_job&& other) noexcept
      : moves(std::exchange(other.moves, nullptr)), calls(other.calls) {
    if (moves)
      moves->fetch_add(1);
  }

  void operator()() { calls->fetch_add(1); }
};

struct lifetime_counter {
  std::atomic<int> registered{};
  std::atomic<int> finished{};

  void register_task() noexcept { registered.fetch_add(1); }

  void finish_task() noexcept { finished.fetch_add(1); }
};

struct large_job {
  inline static std::atomic<int> allocated{};
  std::array<std::byte, 4096> payload{};
  std::shared_ptr<int> resource;

  explicit large_job(std::shared_ptr<int> owned) : resource(std::move(owned)) {}

  large_job(const large_job&) = delete;
  large_job(large_job&&) = default;

  static void* operator new(std::size_t size) {
    void* memory = ::operator new(size);
    allocated.fetch_add(1);
    return memory;
  }

  static void operator delete(void* memory) noexcept {
    allocated.fetch_sub(1);
    ::operator delete(memory);
  }

  int operator()() const { return *resource; }
};

struct throwing_copy_job {
  faio::execution::blocking_executor* service;
  bool allocation_error;

  throwing_copy_job(faio::execution::blocking_executor& executor, bool bad_allocation)
      : service(&executor), allocation_error(bad_allocation) {}

  throwing_copy_job(const throwing_copy_job& other)
      : service(other.service), allocation_error(other.allocation_error) {
    // 可以重新读取服务：潜在抛出的用户构造必须在队列锁外执行。
    (void)service->queued();
    if (allocation_error)
      throw std::bad_alloc{};
    throw std::runtime_error("copy failed");
  }

  throwing_copy_job(throwing_copy_job&&) noexcept = default;

  void operator()() const {}
};
}  // namespace

TEST(ExecutorOptimizationTest, RefConstructsCallableOnlyInAcceptedQueueSlot) {
  faio::execution::blocking_executor service{1, 1};
  faio::execution::blocking_executor_ref executor{service};
  std::binary_semaphore entered{0}, release{0};
  ASSERT_TRUE(executor.try_submit([&] {
    entered.release();
    release.acquire();
  }));
  entered.acquire();

  std::atomic<int> moves{}, calls{};
  counted_job job{moves, calls};
  const auto accepted = executor.try_submit(std::move(job));
  EXPECT_TRUE(accepted);
  // 一个移动进入队列；引用适配层不构造或移动临时类型擦除对象。
  EXPECT_EQ(moves.load(), 1);
  std::atomic<int> rejected_moves{}, rejected_calls{};
  counted_job rejected{rejected_moves, rejected_calls};
  const auto full = executor.try_submit(std::move(rejected));
  EXPECT_FALSE(full);
  if (!full)
    EXPECT_EQ(full.error().value(), EAGAIN);
  EXPECT_EQ(rejected_moves.load(), 0);
  EXPECT_NE(rejected.moves, nullptr);

  release.release();
  service.close();
  EXPECT_EQ(moves.load(), 2);  // 第二个移动是工作线程领取任务。
  EXPECT_EQ(calls.load(), 1);
  EXPECT_EQ(rejected_calls.load(), 0);
}

TEST(ExecutorOptimizationTest, MissingAndClosedServicesDoNotConsumeCallable) {
  std::atomic<int> moves{}, calls{};
  counted_job job{moves, calls};
  faio::execution::blocking_executor_ref missing;
  const auto absent = missing.try_submit(std::move(job));
  ASSERT_FALSE(absent);
  EXPECT_EQ(absent.error().value(), ECANCELED);
  EXPECT_EQ(moves.load(), 0);

  faio::execution::blocking_executor service{1, 1};
  service.close();
  const auto closed = service.try_submit(std::move(job));
  ASSERT_FALSE(closed);
  EXPECT_EQ(closed.error().value(), ECANCELED);
  EXPECT_EQ(moves.load(), 0);
  EXPECT_EQ(calls.load(), 0);
  faio::move_only_function<void()> owned{std::move(job)};
  moves.store(0);
  const auto erased_rejected = service.try_submit(std::move(owned));
  ASSERT_FALSE(erased_rejected);
  EXPECT_TRUE(owned);
  EXPECT_EQ(moves.load(), 0);
}

TEST(ExecutorOptimizationTest, LargeRejectedCallableRetainsPreviousMoveBeforeAdmissionRule) {
  faio::execution::blocking_executor service{1, 1};
  service.close();
  auto resource = std::make_shared<int>(1);
  std::weak_ptr<int> observer = resource;
  large_job job{std::move(resource)};
  const auto rejected = service.try_submit(std::move(job));
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().value(), ECANCELED);
  EXPECT_FALSE(job.resource);
  EXPECT_TRUE(observer.expired());
  EXPECT_EQ(large_job::allocated.load(), 0);
}

TEST(ExecutorOptimizationTest, ThrowingConstructionStaysOutsideQueueLockAndMapsErrors) {
  faio::execution::blocking_executor service{1, 1};
  throwing_copy_job allocation_failure{service, true};
  const auto memory_error = service.try_submit(allocation_failure);
  ASSERT_FALSE(memory_error);
  EXPECT_EQ(memory_error.error().value(), ENOMEM);
  throwing_copy_job other_failure{service, false};
  const auto other_error = service.try_submit(other_failure);
  ASSERT_FALSE(other_error);
  EXPECT_EQ(other_error.error().value(), EIO);
  EXPECT_EQ(service.queued(), 0u);
}

TEST(ExecutorOptimizationTest, TypedTaskReleasesCaptureWhileResultHandleLives) {
  faio::runtime::detail::blocking_pool pool{1, 5ms};
  lifetime_counter lifetime;
  faio::detail::task_tracker tracker;
  auto resource = std::make_shared<int>(42);
  std::weak_ptr<int> observer = resource;
  auto handle = faio::runtime::detail::start_blocking(
      pool,
      [owned = std::move(resource)] { return std::make_unique<int>(*owned); },
      &tracker,
      {},
      faio::task_lifetime_ref{lifetime});
  pool.close();
  EXPECT_TRUE(observer.expired());
  EXPECT_EQ(*handle.get(), 42);
  EXPECT_EQ(lifetime.registered.load(), 1);
  EXPECT_EQ(lifetime.finished.load(), 1);
  EXPECT_EQ(tracker.pending.load(), 0u);
}

TEST(ExecutorOptimizationTest, QueuedCancellationSkipsCallableAndBalancesLifetime) {
  faio::runtime::detail::blocking_pool pool{1, 5ms, 2};
  std::binary_semaphore entered{0}, release{0};
  ASSERT_TRUE(pool.executor().try_submit([&] {
    entered.release();
    release.acquire();
  }));
  entered.acquire();
  lifetime_counter lifetime;
  faio::detail::task_tracker tracker;
  std::stop_source parent;
  std::atomic<int> calls{};
  auto handle = faio::runtime::detail::start_blocking(
      pool,
      [&] { calls.fetch_add(1); },
      &tracker,
      parent.get_token(),
      faio::task_lifetime_ref{lifetime});
  parent.request_stop();
  release.release();
  EXPECT_THROW(handle.get(), faio::operation_cancelled);
  pool.close();
  EXPECT_EQ(calls.load(), 0);
  EXPECT_EQ(lifetime.registered.load(), 1);
  EXPECT_EQ(lifetime.finished.load(), 1);
  EXPECT_EQ(tracker.pending.load(), 0u);
}

TEST(ExecutorOptimizationTest, LargeCallableStorageIsReleasedWhileResultHandleLives) {
  faio::runtime::detail::blocking_pool pool{1, 5ms};
  std::binary_semaphore entered{0}, release{0};
  ASSERT_TRUE(pool.executor().try_submit([&] {
    entered.release();
    release.acquire();
  }));
  entered.acquire();
  auto resource = std::make_shared<int>(17);
  std::weak_ptr<int> observer = resource;
  auto handle =
      faio::runtime::detail::start_blocking(pool, large_job{std::move(resource)}, nullptr, {}, {});
  EXPECT_EQ(large_job::allocated.load(), 1);
  release.release();
  pool.close();
  EXPECT_EQ(large_job::allocated.load(), 0);
  EXPECT_TRUE(observer.expired());
  EXPECT_EQ(handle.get(), 17);
}

TEST(ExecutorOptimizationTest, RejectedTaskRollsBackLifetimeAndCaptureOwnership) {
  faio::runtime::detail::blocking_pool pool{1, 5ms};
  pool.close();
  lifetime_counter lifetime;
  faio::detail::task_tracker tracker;
  auto resource = std::make_shared<int>(1);
  std::weak_ptr<int> observer = resource;
  EXPECT_THROW((void)faio::runtime::detail::start_blocking(
                   pool,
                   [owned = std::move(resource)] { return *owned; },
                   &tracker,
                   {},
                   faio::task_lifetime_ref{lifetime}),
               std::logic_error);
  EXPECT_TRUE(observer.expired());
  EXPECT_EQ(lifetime.registered.load(), 1);
  EXPECT_EQ(lifetime.finished.load(), 1);
  EXPECT_EQ(tracker.pending.load(), 0u);
}
