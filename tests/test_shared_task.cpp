#include <faio/coroutine/composition.hpp>
#include <faio/runtime/instance.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <vector>

namespace {
faio::task<std::unique_ptr<int>> unique_value(std::atomic<int>& starts) {
  ++starts;
  co_return std::make_unique<int>(37);
}

faio::task<void> inspect(faio::shared_task<std::unique_ptr<int>> observer) {
  auto result = co_await observer.observe_uncancellable();
  EXPECT_EQ(*result.value(), 37);
}

faio::task<void> multicast(std::atomic<int>& starts) {
  auto group = co_await faio::task_group([&](faio::task_group_context& owner) -> faio::task<void> {
    const auto observer = owner.spawn_shared_uncancellable(unique_value(starts));
    for (std::size_t i = 0; i < 64; ++i)
      (void)owner.spawn_observed(inspect(observer));
    co_return;
  });
  EXPECT_EQ(group.children.size(), 65);
  for (const auto& result : group.children)
    EXPECT_EQ(result.status, faio::task_outcome_status::value);
}

faio::task<int> fails() {
  throw std::runtime_error("shared failure");
  co_return 0;
}

faio::task<void> repeated_error() {
  auto group = co_await faio::task_group({faio::group_error_policy::collect_all},
                                         [](faio::task_group_context& owner) -> faio::task<void> {
                                           const auto observer =
                                               owner.spawn_shared_uncancellable(fails());
                                           auto first = co_await observer.observe_uncancellable();
                                           auto second = co_await observer.observe_uncancellable();
                                           EXPECT_THROW((void)first.value(), std::runtime_error);
                                           EXPECT_THROW((void)second.value(), std::runtime_error);
                                           EXPECT_EQ(first.exception(), second.exception());
                                         });
  EXPECT_EQ(group.children.size(), 1);
  if (group.children.empty())
    co_return;
  EXPECT_EQ(group.children[0].status, faio::task_outcome_status::error);
}

TEST(CoroutineSharedTask, MulticastAndRepeatableError) {
  auto config = faio::config_builder{}.set_num_workers(4).build();
  faio::runtime::multi_thread_runtime runtime{config};
  std::atomic<int> starts{};
  runtime.block_on(multicast(starts));
  EXPECT_EQ(starts, 1);
  runtime.block_on(repeated_error());
}
}  // namespace
