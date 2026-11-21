#include <faio/coroutine/composition.hpp>
#include <faio/runtime/instance.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
faio::task<void> serial() {
  faio::serial_domain domain;
  std::atomic<int> inside{};
  std::atomic<int> completed{};
  (co_await faio::task_group([&](faio::task_group_context& group) -> faio::task<void> {
    for (int i = 0; i < 32; ++i) {
      (void)group.spawn_observed(domain.run([&]() -> faio::task<void> {
        EXPECT_EQ(inside.fetch_add(1), 0);
        co_await faio::this_coro::yield();
        EXPECT_EQ(inside.fetch_sub(1), 1);
        ++completed;
      }));
    }
    co_return;
  })).body.value_or_throw();
  co_await domain.close();
  EXPECT_EQ(completed, 32);
  try {
    co_await domain.run([]() -> faio::task<void> { co_return; });
    ADD_FAILURE() << "closed domain accepted new work";
  } catch (const std::logic_error&) {
  }
}

TEST(CoroutineSerial, HoldsExclusionAcrossYield) {
  faio::runtime::multi_thread_runtime runtime{faio::config_builder{}.set_num_workers(4).build()};
  runtime.block_on(serial());
}
}  // namespace
