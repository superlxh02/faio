#include "backend_test_support.hpp"
#include <gtest/gtest.h>

#include "faio/faio.hpp"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
auto return_value_task() -> faio::task<int> {
  co_return 42;
}

auto child_increment(std::atomic<int>& counter) -> faio::task<void> {
  counter.fetch_add(1, std::memory_order_relaxed);
  co_return;
}

auto spawn_children(std::atomic<int>& counter) -> faio::task<void> {
  faio::spawn_detached(child_increment(counter));
  faio::spawn_detached(child_increment(counter));
  co_return;
}

auto compute_one() -> faio::task<int> {
  co_return 1;
}

auto compute_two() -> faio::task<int> {
  co_return 2;
}

auto await_blocking(std::atomic<std::size_t>& worker) -> faio::task<int> {
  worker.store(co_await faio::this_coro::worker_id(), std::memory_order_relaxed);
  const auto io_thread = std::this_thread::get_id();
  auto result = faio::spawn_blocking([value = std::make_unique<int>(40), io_thread] {
    if (std::this_thread::get_id() == io_thread)
      throw std::logic_error("阻塞工作在 I/O worker 上运行");
    return *value + 2;
  });
  co_return co_await result;
}
}  // namespace

TEST(RuntimeTaskTest, BlockOnReturnsValue) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().build()};
  const auto value = ctx.block_on(return_value_task());
  EXPECT_EQ(value, 42);
}

TEST(RuntimeTaskTest, BlockingCallableReturnsToCoroutine) {
  faio::runtime::detail::runtime_context ctx{
      faio_test::config_builder().set_num_workers(1).build()};
  std::atomic<std::size_t> worker{faio::detail::no_worker_id};
  EXPECT_EQ(ctx.block_on(await_blocking(worker)), 42);
  EXPECT_NE(worker.load(std::memory_order_relaxed), faio::detail::no_worker_id);
}

TEST(RuntimeTaskTest, BlockingCallableSupportsExternalGetAndExceptions) {
  faio::runtime::detail::runtime_context ctx{
      faio_test::config_builder().set_num_workers(1).build()};
  auto value = ctx.submit_blocking([] { return 7; });
  EXPECT_EQ(value.get(), 7);
  auto failure = ctx.submit_blocking([]() -> int { throw std::runtime_error("blocking failure"); });
  EXPECT_THROW(failure.get(), std::runtime_error);
}

TEST(RuntimeTaskTest, SpawnIsTrackedByBlockOn) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().build()};
  std::atomic<int> counter{0};
  ctx.block_on(spawn_children(counter));
  EXPECT_EQ(counter.load(std::memory_order_relaxed), 2);
}

TEST(RuntimeTaskTest, WaitAllAggregatesResults) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().build()};
  auto [a, b] = ctx.wait_all(compute_one(), compute_two());
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 2);
}

TEST(RuntimeTaskTest, ConfigBuilderAppliesValues) {
  auto cfg = faio_test::config_builder()
                 .set_num_events(2048)
                 .set_num_workers(2)
                 .set_submit_interval(3)
                 .set_io_interval(5)
                 .set_global_queue_interval(7)
                 .build();

  EXPECT_EQ(cfg._num_events, 2048u);
  EXPECT_EQ(cfg._num_workers, 2u);
  EXPECT_EQ(cfg._submit_interval, 3u);
  EXPECT_EQ(cfg._io_interval, 5u);
  EXPECT_EQ(cfg._global_queue_interval, 7u);
}

/** @brief 后端自动空闲策略在启动前解析，显式次数不依赖构建器设置顺序。 */
TEST(RuntimeTaskTest, IdleSpinPolicyResolvesDefaultsAndPreservesExplicitCounts) {
  using faio::runtime::detail::validate_config;
#if defined(__linux__)
  using faio::runtime::io_backend;
  const auto automatic_epoll = faio::ConfigBuilder{}.set_io_backend(io_backend::IO_EPOLL).build();
  EXPECT_EQ(validate_config(automatic_epoll)._idle_spin_count, 0u);
  // 第二次验证保持第一次的已解析值，不把禁用空闲轮询误解为非法配置。
  EXPECT_EQ(validate_config(validate_config(automatic_epoll))._idle_spin_count, 0u);
  for (const auto count : {0u, 4u, 32u}) {
    const auto before_backend = faio::ConfigBuilder{}
                                    .set_idle_spin_count(count)
                                    .set_io_backend(io_backend::IO_EPOLL)
                                    .build();
    const auto after_backend = faio::ConfigBuilder{}
                                   .set_io_backend(io_backend::IO_EPOLL)
                                   .set_idle_spin_count(count)
                                   .build();
    EXPECT_EQ(validate_config(before_backend)._idle_spin_count, count);
    EXPECT_EQ(validate_config(after_backend)._idle_spin_count, count);
  }
#if defined(FAIO_HAS_IO_URING) && FAIO_HAS_IO_URING
  // 编译了 uring 仍可能运行于 <5.10 内核；只在可选择原生后端时验证其默认值。
  if (faio::io::detail::resolve_io_backend(std::nullopt) == io_backend::IO_URING) {
    const auto automatic_native =
        faio::ConfigBuilder{}.set_io_backend(io_backend::IO_URING).build();
    EXPECT_EQ(validate_config(automatic_native)._idle_spin_count, 0u);
    EXPECT_EQ(validate_config(faio::ConfigBuilder{}
                                  .set_io_backend(io_backend::IO_URING)
                                  .set_idle_spin_count(0)
                                  .build())
                  ._idle_spin_count,
              0u);
    EXPECT_EQ(validate_config(faio::ConfigBuilder{}
                                  .set_idle_spin_count(32)
                                  .set_io_backend(io_backend::IO_URING)
                                  .build())
                  ._idle_spin_count,
              32u);
  }
#endif
#else
  EXPECT_EQ(validate_config(faio::ConfigBuilder{}.build())._idle_spin_count, 32u);
  EXPECT_EQ(validate_config(faio::ConfigBuilder{}.set_idle_spin_count(0).build())._idle_spin_count,
            0u);
#endif
}

TEST(RuntimeTaskTest, DefaultRuntimeWorksAcrossExternalThreads) {
  faio::runtime::configure(faio_test::config_builder().set_num_workers(2).build());
  EXPECT_EQ(faio::block_on(return_value_task()), 42);
  auto [first, second] = faio::block_on(faio::join(compute_one(), compute_two()));
  EXPECT_EQ(first + second, 3);
  int result = 0;
  std::thread submitter([&] {
    auto handle = faio::spawn(return_value_task());
    result = handle.get();
  });
  submitter.join();
  EXPECT_EQ(result, 42);
  EXPECT_THROW(faio::runtime::configure(faio_test::config_builder().build()), std::logic_error);
}
