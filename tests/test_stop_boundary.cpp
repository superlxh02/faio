/**
 * @file test_stop_boundary.cpp
 * @brief 测试局部停止、首个终态的仲裁，以及返回前等待任务退出的约定。
 */
#include <faio/coroutine/composition.hpp>
#include <faio/coroutine/lifecycle.hpp>
#include <faio/detail/coroutine/wait/completion_event.hpp>
#include <faio/detail/sync/latch.hpp>
#include <faio/runtime/default.hpp>
#include <faio/runtime/instance.hpp>
#include <gtest/gtest.h>
#include <exception>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace {
using faio::deadline_status;
using faio::runtime::current_thread_runtime;
using faio::task;
using faio::detail::completion_event;
using faio::sync::latch;

task<void> wait_on(latch& gate) {
  co_await gate.wait();
}

task<void> wait_on(latch& gate, completion_event& entered, std::stop_token& token) {
  token = co_await faio::this_coro::stop_token();
  entered.notify();
  co_await gate.wait();
}

task<bool> sees_stop(int& starts) {
  ++starts;
  co_return (co_await faio::this_coro::stop_token()).stop_requested();
}

TEST(CoroutineStopBoundary, PreRequestedExtraTokenIsVisibleWhenLazyChildStarts) {
  current_thread_runtime runtime;
  std::stop_source extra;
  extra.request_stop();
  int starts = 0;

  {
    auto cold = faio::with_stop_token(extra.get_token(), sees_stop(starts));
    EXPECT_EQ(starts, 0);
  }
  EXPECT_EQ(starts, 0);
  EXPECT_TRUE(runtime.block_on(faio::with_stop_token(extra.get_token(), sees_stop(starts))));
  EXPECT_EQ(starts, 1);
}

enum class request_origin { parent, extra };

task<void> check_stop_sources(request_origin origin) {
  latch gate{1};
  completion_event entered;
  std::stop_source extra;
  const auto upstream = co_await faio::this_coro::stop_token();
  std::stop_token actual;
  auto operation =
      faio::spawn(faio::with_stop_token(extra.get_token(), wait_on(gate, entered, actual)));

  co_await entered.wait();
  EXPECT_TRUE(actual.stop_possible());
  EXPECT_NE(actual, extra.get_token());
  EXPECT_NE(actual, operation.stop_token());
  EXPECT_FALSE(actual.stop_requested());
  if (origin == request_origin::parent) {
    operation.request_stop();
    EXPECT_FALSE(extra.stop_requested());
  } else {
    extra.request_stop();
    EXPECT_FALSE(operation.stop_token().stop_requested());
  }
  EXPECT_FALSE(upstream.stop_requested());
  EXPECT_TRUE(actual.stop_requested());
  EXPECT_FALSE(gate.try_wait());

  try {
    co_await operation;
    ADD_FAILURE() << "the cancellation-aware child should throw operation_cancelled";
  } catch (const faio::operation_cancelled&) {
  }
}

TEST(CoroutineStopBoundary, EitherSourceCancelsActualWaitWithoutStoppingTheOther) {
  current_thread_runtime runtime;
  runtime.block_on(check_stop_sources(request_origin::parent));
  runtime.block_on(check_stop_sources(request_origin::extra));
}

task<int> hold_value(completion_event& gate, completion_event& entered, std::stop_token& token) {
  token = co_await faio::this_coro::stop_token();
  entered.notify();
  co_await gate.wait();
  co_return 19;
}

TEST(CoroutineStopBoundary, RequestDoesNotPreemptAnEventuallySuccessfulChild) {
  current_thread_runtime runtime;
  const auto probe = []() -> task<void> {
    std::stop_source extra;
    completion_event entered;
    completion_event gate;
    std::stop_token actual;
    auto operation =
        faio::spawn(faio::with_stop_token(extra.get_token(), hold_value(gate, entered, actual)));
    co_await entered.wait();
    extra.request_stop();
    EXPECT_TRUE(actual.stop_requested());
    EXPECT_FALSE(operation.done());
    gate.notify();
    EXPECT_EQ(co_await operation, 19);
  };
  runtime.block_on(probe());
}

task<std::unique_ptr<int>> move_only_value() {
  co_return std::make_unique<int>(41);
}

task<void> no_result() {
  co_return;
}

TEST(CoroutineDeadline, WorkWinnerReturnsMoveOnlyAndVoidValuesOnce) {
  current_thread_runtime runtime;
  latch absent_trigger{1};
  auto result = runtime.block_on(faio::complete_before(move_only_value(), wait_on(absent_trigger)));
  EXPECT_EQ(result.status(), deadline_status::value);
  EXPECT_EQ(result.exception(), nullptr);
  const auto value = result.take_value();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 41);
  EXPECT_THROW((void)result.take_value(), std::logic_error);

  auto void_result = runtime.block_on(faio::complete_before(no_result(), wait_on(absent_trigger)));
  EXPECT_EQ(void_result.status(), deadline_status::value);
  EXPECT_NO_THROW(void_result.take_value());
  EXPECT_THROW(void_result.take_value(), std::logic_error);
}

task<void> fail_with(std::exception_ptr exception) {
  std::rethrow_exception(exception);
  co_return;
}

TEST(CoroutineDeadline, BothErrorChannelsPreserveExactExceptionIdentity) {
  current_thread_runtime runtime;
  latch no_result_gate{1};
  const auto work_error = std::make_exception_ptr(29);
  const auto trigger_error = std::make_exception_ptr(std::runtime_error("trigger failure"));
  const auto work_result =
      runtime.block_on(faio::complete_before(fail_with(work_error), wait_on(no_result_gate)));
  EXPECT_EQ(work_result.status(), deadline_status::work_error);
  EXPECT_EQ(work_result.exception(), work_error);
  const auto trigger_result =
      runtime.block_on(faio::complete_before(wait_on(no_result_gate), fail_with(trigger_error)));
  EXPECT_EQ(trigger_result.status(), deadline_status::trigger_error);
  EXPECT_EQ(trigger_result.exception(), trigger_error);
}

struct scoped_borrow {
  unsigned& active;

  explicit scoped_borrow(unsigned& count) : active(count) { ++active; }

  ~scoped_borrow() { --active; }
};

enum class winning_branch { work, trigger };

task<void> check_loser_cleanup(winning_branch winner) {
  latch winning_gate{1};
  latch losing_gate{1};
  latch cleanup_gate{1};
  completion_event loser_entered;
  completion_event cleanup_entered;
  std::stop_token loser_token;
  unsigned borrows = 0;
  auto delayed =
      faio::with_cleanup([&] { return wait_on(losing_gate, loser_entered, loser_token); },
                         [&](const faio::task_exit& exit) -> task<void> {
                           EXPECT_EQ(exit.status, faio::task_exit::kind::cancelled);
                           EXPECT_NE(exit.exception, nullptr);
                           scoped_borrow borrow{borrows};
                           cleanup_entered.notify();
                           co_await cleanup_gate.wait();
                         });
  auto operation =
      winner == winning_branch::work
          ? faio::spawn(faio::complete_before(wait_on(winning_gate), std::move(delayed)))
          : faio::spawn(faio::complete_before(std::move(delayed), wait_on(winning_gate)));

  co_await loser_entered.wait();
  EXPECT_FALSE(loser_token.stop_requested());
  winning_gate.count_down();
  co_await cleanup_entered.wait();
  EXPECT_TRUE(loser_token.stop_requested());
  EXPECT_EQ(borrows, 1U);
  EXPECT_FALSE(operation.done());
  cleanup_gate.count_down();

  const auto result = co_await operation;
  EXPECT_EQ(result.status(),
            winner == winning_branch::work ? deadline_status::value : deadline_status::deadline);
  EXPECT_EQ(result.exception(), nullptr);
  EXPECT_EQ(borrows, 0U);
}

TEST(CoroutineDeadline, TriggerWinnerDrainsWorkCleanupAndReleasesBorrow) {
  current_thread_runtime runtime;
  runtime.block_on(check_loser_cleanup(winning_branch::trigger));
}

TEST(CoroutineDeadline, WorkWinnerDrainsTriggerCleanupAndReleasesBorrow) {
  current_thread_runtime runtime;
  runtime.block_on(check_loser_cleanup(winning_branch::work));
}

task<void> check_stopped_first(winning_branch first) {
  completion_event cooperative_entered;
  completion_event other_entered;
  completion_event cooperative_exits;
  completion_event release_other;
  latch cancellable{1};
  std::stop_token cooperative_token;
  std::stop_token other_token;

  const auto cooperate = [&]() -> task<void> {
    cooperative_token = co_await faio::this_coro::stop_token();
    cooperative_entered.notify();
    try {
      co_await cancellable.wait();
    } catch (const faio::operation_cancelled&) {
      cooperative_exits.notify();
      throw;
    }
  };
  const auto finish_later = [&]() -> task<void> {
    other_token = co_await faio::this_coro::stop_token();
    other_entered.notify();
    co_await release_other.wait();
  };
  auto operation = first == winning_branch::work
                       ? faio::spawn(faio::complete_before(cooperate(), finish_later()))
                       : faio::spawn(faio::complete_before(finish_later(), cooperate()));

  co_await cooperative_entered.wait();
  co_await other_entered.wait();
  operation.request_stop();
  EXPECT_TRUE(cooperative_token.stop_requested());
  EXPECT_TRUE(other_token.stop_requested());
  co_await cooperative_exits.wait();
  EXPECT_FALSE(operation.done());
  release_other.notify();

  const auto result = co_await operation;
  EXPECT_EQ(result.status(), deadline_status::stopped);
  EXPECT_NE(result.exception(), nullptr);
}

TEST(CoroutineDeadline, EitherBranchCanPublishStoppedBeforeTheOtherExits) {
  current_thread_runtime runtime;
  runtime.block_on(check_stopped_first(winning_branch::work));
  runtime.block_on(check_stopped_first(winning_branch::trigger));
}

TEST(CoroutineDeadline, RestrictedTriggerAdmissionFailureStopsAndDrainsAcceptedWork) {
  current_thread_runtime runtime{faio::config_builder{}.set_no_stealing_queue_capacity(2).build()};
  latch work_gate{1};
  unsigned work_starts = 0;
  unsigned trigger_starts = 0;
  unsigned cleanup_starts = 0;
  unsigned cleanup_exits = 0;
  unsigned borrows = 0;
  bool observed_stop = false;

  const auto work = [&]() -> task<void> {
    ++work_starts;
    observed_stop = (co_await faio::this_coro::stop_token()).stop_requested();
    co_await work_gate.wait();
  };
  const auto cleanup = [&](const faio::task_exit& exit) -> task<void> {
    ++cleanup_starts;
    EXPECT_EQ(exit.status, faio::task_exit::kind::cancelled);
    EXPECT_NE(exit.exception, nullptr);
    scoped_borrow borrow{borrows};
    co_await faio::this_coro::yield();
    ++cleanup_exits;
  };
  const auto trigger = [&]() -> task<void> {
    ++trigger_starts;
    co_return;
  };

  auto operation =
      faio::complete_before(faio::with_cleanup(work, cleanup), trigger()).with_no_stealing();
  EXPECT_THROW((void)runtime.block_on(std::move(operation)), std::overflow_error);
  EXPECT_EQ(work_starts, 1U);
  EXPECT_EQ(trigger_starts, 0U);
  EXPECT_TRUE(observed_stop);
  EXPECT_FALSE(work_gate.try_wait());
  EXPECT_EQ(cleanup_starts, 1U);
  EXPECT_EQ(cleanup_exits, 1U);
  EXPECT_EQ(borrows, 0U);

  const auto group = [](faio::task_group_context& context) -> task<void> {
    (void)context.spawn_observed(no_result());
    co_return;
  };
  auto subsequent = runtime.block_on(faio::task_group(group).with_no_stealing());
  EXPECT_EQ(subsequent.body.status(), faio::task_outcome_status::value);
  ASSERT_EQ(subsequent.children.size(), 1U);
  EXPECT_EQ(subsequent.children.front().status, faio::task_outcome_status::value);
}
}  // namespace
