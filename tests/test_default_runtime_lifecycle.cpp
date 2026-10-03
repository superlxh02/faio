#include "backend_test_support.hpp"
#include "faio/faio.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
faio::task<void> increment_after_yield(std::atomic<int>& completed) {
  co_await faio::this_coro::yield();
  completed.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

TEST(DefaultRuntimeTest, DrainsExternalSubmissionsAndRejectsLateTasks) {
  EXPECT_THROW(faio::runtime::configure(
                   faio_test::config_builder().set_num_workers(0).build()),
               std::invalid_argument);
  faio::runtime::configure(faio_test::config_builder().set_num_workers(2).build());
  std::atomic<int> completed{0};
  std::vector<std::thread> submitters;
  for (int thread = 0; thread < 4; ++thread) {
    submitters.emplace_back([&] {
      for (int i = 0; i < 100; ++i)
        faio::spawn_detached(increment_after_yield(completed));
    });
  }
  for (auto& thread : submitters) thread.join();
  faio::runtime::shutdown();
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 400);
  EXPECT_THROW((void)faio::spawn(increment_after_yield(completed)), std::logic_error);
  EXPECT_THROW(faio::block_on(increment_after_yield(completed)), std::logic_error);
}
