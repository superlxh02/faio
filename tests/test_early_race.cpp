#include <faio/coroutine/composition.hpp>
#include <faio/runtime/instance.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <vector>

namespace {
faio::task<int> value(int n) {
  co_return n;
}

faio::task<int> late(faio::detail::completion_event& release, std::atomic<bool>& exited) {
  co_await release.wait();
  EXPECT_TRUE((co_await faio::this_coro::stop_token()).stop_requested());
  exited = true;
  throw std::logic_error("late loser");
}

faio::task<void> first_result() {
  faio::detail::completion_event release;
  std::atomic<bool> exited{};
  auto group = co_await faio::task_group(
      [&](faio::task_group_context& owner) -> faio::task<faio::race_result<int, int>> {
        auto result = co_await faio::race_in(owner, value(9), late(release, exited));
        EXPECT_EQ(result.index, 0);
        EXPECT_EQ(std::get<0>(result.outcome).take_value(), 9);
        EXPECT_FALSE(exited);
        EXPECT_FALSE(result.loser_error(1));
        release.notify();
        co_return result;
      });
  EXPECT_TRUE(exited);
  EXPECT_TRUE(group.children.empty());
  auto result = group.body.take_value();
  auto error = result.loser_error(1);
  EXPECT_TRUE(error);
  if (!error)
    co_return;
  EXPECT_THROW(std::rethrow_exception(error), std::logic_error);
}

faio::task<void> rejected_race() {
  auto group = co_await faio::task_group([](faio::task_group_context& owner) -> faio::task<void> {
    owner.close();
    try {
      (void)co_await faio::race_in(owner, value(42));
      ADD_FAILURE() << "closed group accepted the race";
    } catch (const std::system_error& error) {
      EXPECT_EQ(error.code(), faio::make_error_code(faio::task_group_errc::closed));
    }
  });
  EXPECT_TRUE(group.children.empty());
}

TEST(CoroutineEarlyRace, ReadyBeforeLoserAndDrainBeforeGroupReturn) {
  faio::runtime::current_thread_runtime runtime;
  runtime.block_on(first_result());
  runtime.block_on(rejected_race());
}
}  // namespace
