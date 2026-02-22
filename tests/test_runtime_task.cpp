#include <gtest/gtest.h>

#include "faio/faio.hpp"

#include <atomic>

namespace {

auto return_value_task() -> faio::task<int> {
  co_return 42;
}

auto child_increment(std::atomic<int>& counter) -> faio::task<void> {
  counter.fetch_add(1, std::memory_order_relaxed);
  co_return;
}

auto spawn_children(std::atomic<int>& counter) -> faio::task<void> {
  faio::spawn(child_increment(counter));
  faio::spawn(child_increment(counter));
  co_return;
}

auto compute_one() -> faio::task<int> {
  co_return 1;
}

auto compute_two() -> faio::task<int> {
  co_return 2;
}

}  // namespace

TEST(RuntimeTaskTest, BlockOnReturnsValue) {
  faio::runtime_context ctx;
  const auto value = faio::block_on(ctx, return_value_task());
  EXPECT_EQ(value, 42);
}

TEST(RuntimeTaskTest, SpawnIsTrackedByBlockOn) {
  faio::runtime_context ctx;
  std::atomic<int> counter{0};
  faio::block_on(ctx, spawn_children(counter));
  EXPECT_EQ(counter.load(std::memory_order_relaxed), 2);
}

TEST(RuntimeTaskTest, WaitAllAggregatesResults) {
  faio::runtime_context ctx;
  auto [a, b] = faio::wait_all(ctx, compute_one(), compute_two());
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 2);
}

TEST(RuntimeTaskTest, ConfigBuilderAppliesValues) {
  auto cfg = faio::ConfigBuilder{}
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
