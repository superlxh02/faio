/**
 * @file test_coroutine.cpp
 * @brief task_outcome 的单次结果消费与 settle 的边界行为。
 */
#include <faio/coroutine/composition.hpp>
#include <faio/coroutine/lifecycle.hpp>
#include <faio/detail/coroutine/context/execution_binding.hpp>
#include <faio/detail/coroutine/lifetime/reference.hpp>
#include <faio/detail/coroutine/wait/completion_event.hpp>
#include <faio/detail/sync/latch.hpp>
#include <faio/runtime/default.hpp>
#include <faio/runtime/instance.hpp>
#include <gtest/gtest.h>
#include <array>
#include <algorithm>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {
using faio::task;
using faio::task_outcome;
using status = faio::task_outcome_status;
using faio::runtime::current_thread_runtime;

/** @brief 记录唯一待恢复句柄，由测试显式驱动。 */
struct test_scheduler {
  std::coroutine_handle<> pending;

  /** @brief 模拟单线程调度器的入队操作。 */
  void enqueue(std::coroutine_handle<> work) { pending = work; }
};

namespace detail = faio::detail;

static_assert(std::same_as<decltype(faio::settle(std::declval<task<int, test_scheduler>>())),
                           task<task_outcome<int>, test_scheduler>>);

/** @brief 仅在任务实际启动时记录，并返回普通值。 */
task<int> observed_value(int& starts) {
  ++starts;
  co_return 42;
}

/** @brief 验证仍使用原静态调度器，并通过 yield 实际投递一次。 */
task<int, test_scheduler> value_on_custom_scheduler(test_scheduler& scheduler) {
  const auto bound = co_await faio::this_coro::scheduler();
  EXPECT_EQ(bound.native_handle(), &scheduler);
  co_await faio::this_coro::yield();
  co_return 31;
}

/** @brief 正常完成一个无值任务。 */
task<void> returns_void() {
  co_return;
}

/** @brief 抛出标准异常，供外层捕获。 */
task<int> throws_standard() {
  throw std::runtime_error("child error");
  co_return 0;
}

/** @brief 抛出标准异常体系以外的异常，供外层捕获。 */
task<void> throws_nonstandard() {
  throw 123;
  co_return;
}

/** @brief 直接抛出取消异常，由调用方判断是否有匹配的停止请求。 */
task<void> throws_cancelled() {
  throw faio::operation_cancelled{};
  co_return;
}

/** @brief 返回业务层 expected，通过正常返回值传递业务错误。 */
task<std::expected<int, int>> expected_value() {
  co_return std::unexpected{7};
}

/** @brief 构造只能移动的成功结果。 */
task<std::unique_ptr<int>> unique_value() {
  co_return std::make_unique<int>(17);
}

/** @brief 分别记录各阶段的移动次数。 */
struct move_control {
  int moves{};
  int throw_on{};
  bool cancellation_error{};
};

/** @brief 能在指定移动阶段失败且不可复制的结果。 */
struct staged_value {
  move_control& control;

  explicit staged_value(move_control& state) noexcept : control(state) {}

  staged_value(const staged_value&) = delete;
  void* operator new(std::size_t) = delete;

  staged_value(staged_value&& previous) : control(previous.control) {
    ++control.moves;
    if (control.moves != control.throw_on)
      return;
    if (control.cancellation_error)
      throw faio::operation_cancelled{};
    throw std::runtime_error("result move");
  }
};

/** @brief 直接用构造实参发布值，首次移动发生在父协程恢复时。 */
task<staged_value> result_move(move_control& control) {
  co_return control;
}

/** @brief 先记录实际使用的运行时令牌并报告即将等待，再等待可取消的门闩。 */
task<void> wait_until_cancelled(faio::sync::latch& entered,
                                faio::sync::latch& gate,
                                std::stop_token& child_token) {
  child_token = co_await faio::this_coro::stop_token();
  entered.count_down();
  co_await gate.wait();
}

/** @brief 不可取消地等待正常完成，检查该任务收到停止请求后仍返回业务值。 */
task<int> return_after_stop(faio::sync::latch& entered, faio::detail::completion_event& gate) {
  const auto child_token = co_await faio::this_coro::stop_token();
  entered.count_down();
  co_await gate.wait();
  EXPECT_TRUE(child_token.stop_requested());
  co_return 19;
}

/** @brief 测试可取消的等待是否实际停止，以及取消异常的转换。 */
task<void> check_cancellable_wait() {
  faio::sync::latch entered{1};
  faio::sync::latch gate{1};
  std::stop_token child_token;
  auto job = faio::spawn(faio::settle(wait_until_cancelled(entered, gate, child_token)));

  co_await entered.wait();
  EXPECT_TRUE(child_token.stop_possible());
  EXPECT_EQ(child_token, job.stop_token());
  EXPECT_FALSE(job.done());
  job.request_stop();
  EXPECT_FALSE(gate.try_wait());

  auto result = co_await job;
  EXPECT_TRUE(child_token.stop_requested());
  EXPECT_EQ(result.status(), status::stopped);
  EXPECT_NE(result.exception(), nullptr);
  EXPECT_THROW(result.value_or_throw(), faio::operation_cancelled);
  EXPECT_EQ(result.status(), status::stopped);
}

/** @brief 停止请求不提前交付未完成的子任务。 */
task<void> check_normal_value_after_stop() {
  faio::sync::latch entered{1};
  faio::detail::completion_event gate;
  auto job = faio::spawn(faio::settle(return_after_stop(entered, gate)));

  co_await entered.wait();
  EXPECT_FALSE(job.done());
  job.request_stop();
  EXPECT_FALSE(job.done());
  gate.notify();

  auto result = co_await job;
  EXPECT_EQ(result.status(), status::value);
  EXPECT_EQ(result.exception(), nullptr);
  EXPECT_EQ(result.take_value(), 19);
}

/** @brief 已请求停止时，区分子任务的实际终态与提取值时抛出的取消异常。 */
task<void> check_cancel_shaped_transfer_error(move_control& control) {
  auto job = faio::spawn(faio::settle(result_move(control)));
  job.request_stop();
  auto result = co_await job;
  EXPECT_EQ(result.status(), status::error);
  EXPECT_NE(result.exception(), nullptr);
  EXPECT_THROW(result.value_or_throw(), faio::operation_cancelled);
  EXPECT_EQ(control.moves, control.throw_on);
}

/** @brief 提交的根任务和入口根任务会在 block_on 返回前完成注销。 */
TEST(CoroutineLifetime, BlockOnReclaimsRegisteredRoots) {
  current_thread_runtime executor;
  faio::task_lifetime_ref roots;
  int starts = 0;
  executor.block_on([](faio::task_lifetime_ref& roots, int& starts) -> task<void> {
    roots = detail::current_task_lifetime();
    auto work = faio::spawn(observed_value(starts));
    EXPECT_TRUE(roots);
    if (roots)
      EXPECT_GE(roots.native_handle()->count(), 2u);
    EXPECT_EQ(co_await work, 42);
  }(roots, starts));

  ASSERT_TRUE(roots);
  EXPECT_EQ(roots.native_handle()->count(), 0u);
  EXPECT_EQ(starts, 1);
}

TEST(CoroutineOutcome, ValueAndVoidAreConsumedOnlyOnce) {
  auto result = task_outcome<std::unique_ptr<int>>::from_value(std::make_unique<int>(13));
  EXPECT_EQ(result.status(), status::value);
  EXPECT_EQ(result.exception(), nullptr);
  const auto value = result.take_value();
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 13);
  EXPECT_EQ(result.status(), status::value);
  EXPECT_THROW((void)result.take_value(), std::logic_error);
  EXPECT_THROW((void)result.value_or_throw(), std::logic_error);

  auto empty_value = task_outcome<void>::from_value();
  EXPECT_EQ(empty_value.status(), status::value);
  EXPECT_EQ(empty_value.exception(), nullptr);
  EXPECT_NO_THROW(empty_value.value_or_throw());
  EXPECT_EQ(empty_value.status(), status::value);
  EXPECT_THROW(empty_value.take_value(), std::logic_error);
}

TEST(CoroutineOutcome, ErrorAndStoppedPreserveDistinctStatesAndExceptionIdentity) {
  const auto failure = std::make_exception_ptr(std::runtime_error("outcome error"));
  auto error = task_outcome<int>::from_error(failure);
  EXPECT_EQ(error.status(), status::error);
  EXPECT_EQ(error.exception(), failure);
  EXPECT_THROW((void)error.value_or_throw(), std::runtime_error);
  EXPECT_EQ(error.status(), status::error);

  const auto cancellation = std::make_exception_ptr(faio::operation_cancelled{});
  auto stopped = task_outcome<void>::from_stopped(cancellation);
  EXPECT_EQ(stopped.status(), status::stopped);
  EXPECT_EQ(stopped.exception(), cancellation);
  EXPECT_THROW(stopped.value_or_throw(), faio::operation_cancelled);
  EXPECT_EQ(stopped.status(), status::stopped);

  auto unexplained = task_outcome<int>::from_stopped();
  EXPECT_EQ(unexplained.status(), status::stopped);
  EXPECT_EQ(unexplained.exception(), nullptr);
  EXPECT_THROW((void)unexplained.value_or_throw(), faio::operation_cancelled);
}

TEST(CoroutineOutcome, ConsumerMoveFailureDoesNotRewritePublishedState) {
  move_control control;
  current_thread_runtime runtime;
  auto result = runtime.block_on(faio::settle(result_move(control)));
  EXPECT_EQ(result.status(), status::value);
  EXPECT_EQ(result.exception(), nullptr);

  control.throw_on = control.moves + 1;
  EXPECT_THROW((void)result.take_value(), std::runtime_error);
  EXPECT_EQ(result.status(), status::value);
  EXPECT_EQ(result.exception(), nullptr);
  EXPECT_EQ(control.moves, control.throw_on);
  EXPECT_THROW((void)result.take_value(), std::logic_error);
  EXPECT_EQ(control.moves, control.throw_on);
}

TEST(CoroutineSettle, WrapperIsLazyAndPreservesValueAndVoid) {
  current_thread_runtime runtime;
  int starts = 0;
  auto settled = faio::settle(observed_value(starts));
  EXPECT_EQ(starts, 0);
  auto value = runtime.block_on(std::move(settled));
  EXPECT_EQ(starts, 1);
  EXPECT_EQ(value.status(), status::value);
  EXPECT_EQ(value.value_or_throw(), 42);

  auto done = runtime.block_on(faio::settle(returns_void()));
  EXPECT_EQ(done.status(), status::value);
  EXPECT_EQ(done.exception(), nullptr);
  EXPECT_NO_THROW(done.value_or_throw());
}

TEST(CoroutineSettle, PreservesCustomSchedulerThroughRealEnqueueAndResume) {
  test_scheduler scheduler;
  detail::scoped_thread_context thread;
  int local_state{};
  detail::execution_thread_binding<test_scheduler> binding{
      faio::basic_scheduler_ref<test_scheduler>{scheduler}, {}, local_state, 0};
  detail::execution_thread_guard attached{binding};

  auto settled = faio::settle(value_on_custom_scheduler(scheduler)).operator co_await();
  ASSERT_FALSE(settled.await_ready());
  auto started = settled.await_suspend(std::noop_coroutine());
  started.resume();
  ASSERT_NE(scheduler.pending, nullptr);

  const auto next = std::exchange(scheduler.pending, {});
  next.resume();
  ASSERT_TRUE(started.done());
  EXPECT_EQ(scheduler.pending, nullptr);
  auto outcome = settled.await_resume();
  EXPECT_EQ(outcome.status(), status::value);
  EXPECT_EQ(outcome.take_value(), 31);
}

TEST(CoroutineSettle, MoveOnlyAndBusinessExpectedAreValues) {
  current_thread_runtime runtime;
  auto outcome = runtime.block_on(faio::settle(unique_value()));
  EXPECT_EQ(outcome.status(), status::value);
  const auto owned = outcome.value_or_throw();
  ASSERT_NE(owned, nullptr);
  EXPECT_EQ(*owned, 17);

  auto expected = runtime.block_on(faio::settle(expected_value()));
  EXPECT_EQ(expected.status(), status::value);
  EXPECT_EQ(expected.exception(), nullptr);
  const auto business_result = expected.take_value();
  ASSERT_FALSE(business_result.has_value());
  EXPECT_EQ(business_result.error(), 7);
}

TEST(CoroutineSettle, ErrorsIncludingUnexpectedCancellationAndEmptyTaskAreObserved) {
  current_thread_runtime runtime;
  auto standard = runtime.block_on(faio::settle(throws_standard()));
  EXPECT_EQ(standard.status(), status::error);
  ASSERT_NE(standard.exception(), nullptr);
  EXPECT_THROW((void)standard.value_or_throw(), std::runtime_error);

  auto raw = runtime.block_on(faio::settle(throws_nonstandard()));
  EXPECT_EQ(raw.status(), status::error);
  try {
    raw.value_or_throw();
    ADD_FAILURE() << "expected the child's nonstandard exception";
  } catch (int code) {
    EXPECT_EQ(code, 123);
  }

  auto unexpected_stop = runtime.block_on(faio::settle(throws_cancelled()));
  EXPECT_EQ(unexpected_stop.status(), status::error);
  EXPECT_THROW(unexpected_stop.value_or_throw(), faio::operation_cancelled);

  auto empty = runtime.block_on(faio::settle(task<int>{}));
  EXPECT_EQ(empty.status(), status::error);
  EXPECT_THROW((void)empty.value_or_throw(), std::logic_error);
  EXPECT_THROW(runtime.block_on(task<int>{}), std::logic_error);
  EXPECT_THROW(runtime.block_on(throws_standard()), std::runtime_error);
  EXPECT_THROW(runtime.block_on(throws_cancelled()), faio::operation_cancelled);
}

TEST(CoroutineSettle, MatchingCancellationBecomesStopped) {
  current_thread_runtime runtime;
  runtime.block_on(check_cancellable_wait());
}

TEST(CoroutineSettle, StopRequestDoesNotPreemptReturningAValue) {
  current_thread_runtime runtime;
  runtime.block_on(check_normal_value_after_stop());
}

TEST(CoroutineSettle, ProducerValueTransferFailuresPublishError) {
  current_thread_runtime runtime;
  move_control ordinary_failure{.throw_on = 2};
  auto error = runtime.block_on(faio::settle(result_move(ordinary_failure)));
  EXPECT_EQ(error.status(), status::error);
  ASSERT_NE(error.exception(), nullptr);
  EXPECT_THROW((void)error.value_or_throw(), std::runtime_error);
  EXPECT_EQ(ordinary_failure.moves, 2);

  for (const int fail_on : {1, 2}) {
    SCOPED_TRACE(fail_on);
    move_control cancellation{.throw_on = fail_on, .cancellation_error = true};
    runtime.block_on(check_cancel_shaped_transfer_error(cancellation));
  }
}

/** @brief 原样重抛已有异常，供调用方检查异常对象身份。 */
task<void> rethrow_task(std::exception_ptr failure) {
  std::rethrow_exception(failure);
  co_return;
}

/** @brief 返回当前协程帧使用的停止令牌。 */
task<std::stop_token> read_task_stop_token() {
  co_return co_await faio::this_coro::stop_token();
}

TEST(CoroutineCleanup, FactoriesAreLazyAndRunOnceAfterStarting) {
  current_thread_runtime runtime;
  int body_calls = 0;
  int cleanup_calls = 0;
  const auto body = [&] {
    ++body_calls;
    return unique_value();
  };
  const auto cleanup = [&](const faio::task_exit& exit) {
    ++cleanup_calls;
    EXPECT_EQ(exit.status, faio::task_exit::kind::succeeded);
    EXPECT_EQ(exit.exception, nullptr);
    return returns_void();
  };
  {
    auto unstarted = faio::with_cleanup(body, cleanup);
    EXPECT_EQ(body_calls, 0);
    EXPECT_EQ(cleanup_calls, 0);
  }
  EXPECT_EQ(body_calls, 0);
  EXPECT_EQ(cleanup_calls, 0);

  auto owned = runtime.block_on(faio::with_cleanup(body, cleanup));
  EXPECT_EQ(body_calls, 1);
  EXPECT_EQ(cleanup_calls, 1);
  ASSERT_NE(owned, nullptr);
  EXPECT_EQ(*owned, 17);
  EXPECT_NO_THROW(runtime.block_on(faio::with_cleanup(returns_void, cleanup)));
  EXPECT_EQ(cleanup_calls, 2);
}

TEST(CoroutineCleanup, SynchronousBodyFactoryFailureStillRunsCleanupExactlyOnce) {
  current_thread_runtime runtime;
  const auto failure = std::make_exception_ptr(std::runtime_error("body factory"));
  int cleanup_calls = 0;
  auto wrapped = faio::with_cleanup([&]() -> task<int> { std::rethrow_exception(failure); },
                                    [&](const faio::task_exit& exit) {
                                      ++cleanup_calls;
                                      EXPECT_EQ(exit.status, faio::task_exit::kind::failed);
                                      EXPECT_EQ(exit.exception, failure);
                                      return returns_void();
                                    });
  try {
    (void)runtime.block_on(std::move(wrapped));
    ADD_FAILURE() << "expected the original body factory failure";
  } catch (const std::runtime_error&) {
    EXPECT_EQ(std::current_exception(), failure);
  }
  EXPECT_EQ(cleanup_calls, 1);
}

TEST(CoroutineCleanup, SynchronousCleanupFactoryFailureSupersedesSuccess) {
  current_thread_runtime runtime;
  const auto failure = std::make_exception_ptr(std::runtime_error("cleanup factory"));
  auto wrapped = faio::with_cleanup(returns_void, [&](const faio::task_exit& exit) -> task<void> {
    EXPECT_EQ(exit.status, faio::task_exit::kind::succeeded);
    EXPECT_EQ(exit.exception, nullptr);
    std::rethrow_exception(failure);
  });
  try {
    runtime.block_on(std::move(wrapped));
    ADD_FAILURE() << "expected the original cleanup factory failure";
  } catch (const std::runtime_error&) {
    EXPECT_EQ(std::current_exception(), failure);
  }
}

TEST(CoroutineCleanup, DoubleFailurePreservesBodyAndCleanupExceptionsInOrder) {
  current_thread_runtime runtime;
  const auto primary = std::make_exception_ptr(33);
  const auto secondary = std::make_exception_ptr(std::runtime_error("cleanup task"));
  int cleanup_calls = 0;
  auto wrapped = faio::with_cleanup([&] { return rethrow_task(primary); },
                                    [&](const faio::task_exit& exit) {
                                      ++cleanup_calls;
                                      EXPECT_EQ(exit.status, faio::task_exit::kind::failed);
                                      EXPECT_EQ(exit.exception, primary);
                                      return rethrow_task(secondary);
                                    });
  try {
    runtime.block_on(std::move(wrapped));
    ADD_FAILURE() << "expected both exceptions";
  } catch (const faio::cleanup_error& failure) {
    EXPECT_EQ(failure.primary(), primary);
    EXPECT_EQ(failure.secondary(), secondary);
  }
  EXPECT_EQ(cleanup_calls, 1);
}

TEST(CoroutineCleanup, UnexpectedCancellationIsAnOrdinaryFailure) {
  current_thread_runtime runtime;
  const auto failure = std::make_exception_ptr(faio::operation_cancelled{});
  auto wrapped = faio::with_cleanup([&] { return rethrow_task(failure); },
                                    [&](const faio::task_exit& exit) {
                                      EXPECT_EQ(exit.status, faio::task_exit::kind::failed);
                                      EXPECT_EQ(exit.exception, failure);
                                      return returns_void();
                                    });
  try {
    runtime.block_on(std::move(wrapped));
    ADD_FAILURE() << "expected the original cancellation-shaped failure";
  } catch (const faio::operation_cancelled&) {
    EXPECT_EQ(std::current_exception(), failure);
  }
}

/** @brief 验证 cleanup 会挂起、沿用词法调度域，且不向子任务传播停止请求。 */
task<void> check_cleanup_waits_after_body_cancellation(bool request_before_start) {
  faio::sync::latch body_entered{1};
  faio::sync::latch body_gate{1};
  faio::sync::latch cleanup_entered{1};
  faio::sync::latch cleanup_gate{1};
  std::stop_token body_token;
  std::stop_token cleanup_token;
  std::exception_ptr body_failure;
  const void* body_scope{};
  bool cleanup_done = false;
  bool body_started_with_stop = false;
  int cleanup_calls = 0;

  auto job = faio::spawn(faio::with_cleanup(
      [&]() -> task<void> {
        body_token = co_await faio::this_coro::stop_token();
        body_scope = detail::current_task_context().scope;
        body_started_with_stop = body_token.stop_requested();
        body_entered.count_down();
        co_await body_gate.wait();
      },
      [&](const faio::task_exit& exit) -> task<void> {
        ++cleanup_calls;
        EXPECT_EQ(exit.status, faio::task_exit::kind::cancelled);
        body_failure = exit.exception;
        cleanup_token = co_await faio::this_coro::stop_token();
        EXPECT_TRUE(cleanup_token.stop_possible());
        EXPECT_FALSE(cleanup_token.stop_requested());
        EXPECT_NE(cleanup_token, body_token);
        EXPECT_EQ(detail::current_task_context().scope, body_scope);
        const auto child_token = co_await read_task_stop_token();
        EXPECT_EQ(child_token, cleanup_token);
        cleanup_entered.count_down();
        co_await cleanup_gate.wait();
        EXPECT_FALSE(exit.exception == nullptr);
        EXPECT_EQ(exit.exception, body_failure);
        EXPECT_FALSE(cleanup_token.stop_requested());
        cleanup_done = true;
      }));

  const auto wrapper_token = job.stop_token();
  if (request_before_start)
    job.request_stop();
  co_await body_entered.wait();
  EXPECT_EQ(body_token, wrapper_token);
  EXPECT_EQ(body_started_with_stop, request_before_start);
  if (!request_before_start)
    job.request_stop();
  EXPECT_TRUE(wrapper_token.stop_requested());
  EXPECT_TRUE(body_token.stop_requested());
  EXPECT_FALSE(body_gate.try_wait());

  co_await cleanup_entered.wait();
  EXPECT_FALSE(job.done());
  EXPECT_FALSE(cleanup_done);
  EXPECT_FALSE(cleanup_token.stop_requested());
  job.request_stop();
  EXPECT_FALSE(cleanup_token.stop_requested());
  cleanup_gate.count_down();

  try {
    co_await job;
    ADD_FAILURE() << "expected the body's original cancellation";
  } catch (const faio::operation_cancelled&) {
    EXPECT_EQ(std::current_exception(), body_failure);
  }
  EXPECT_TRUE(cleanup_done);
  EXPECT_EQ(cleanup_calls, 1);
}

TEST(CoroutineCleanup, BeforeStartAndInFlightStopWaitForIndependentCleanup) {
  current_thread_runtime runtime;
  runtime.block_on(check_cleanup_waits_after_body_cancellation(true));
  runtime.block_on(check_cleanup_waits_after_body_cancellation(false));
}

/** @brief 对仍在正常执行的 body 请求停止，确认 cleanup 不会提前启动或交付结果。 */
task<void> check_stop_without_cancelling_body() {
  faio::sync::latch body_entered{1};
  faio::detail::completion_event body_gate;
  faio::sync::latch cleanup_entered{1};
  faio::sync::latch cleanup_gate{1};
  std::stop_token cleanup_token;
  int cleanup_calls = 0;
  auto job =
      faio::spawn(faio::with_cleanup([&] { return return_after_stop(body_entered, body_gate); },
                                     [&](const faio::task_exit& exit) -> task<void> {
                                       ++cleanup_calls;
                                       EXPECT_EQ(exit.status, faio::task_exit::kind::succeeded);
                                       EXPECT_EQ(exit.exception, nullptr);
                                       cleanup_token = co_await faio::this_coro::stop_token();
                                       cleanup_entered.count_down();
                                       co_await cleanup_gate.wait();
                                     }));

  co_await body_entered.wait();
  job.request_stop();
  EXPECT_FALSE(job.done());
  EXPECT_EQ(cleanup_calls, 0);
  body_gate.notify();

  co_await cleanup_entered.wait();
  job.request_stop();
  EXPECT_TRUE(cleanup_token.stop_possible());
  EXPECT_FALSE(cleanup_token.stop_requested());
  EXPECT_FALSE(job.done());
  cleanup_gate.count_down();
  EXPECT_EQ(co_await job, 19);
  EXPECT_EQ(cleanup_calls, 1);
}

TEST(CoroutineCleanup, RequestedStopAloneDoesNotChangeSuccessfulExit) {
  current_thread_runtime runtime;
  runtime.block_on(check_stop_without_cancelling_body());
}

TEST(CoroutineCleanup, NestedCleanupCompletesInLexicalOrder) {
  current_thread_runtime runtime;
  std::vector<int> events;
  auto wrapped = faio::with_cleanup(
      [&]() -> task<void> {
        events.push_back(1);
        co_await faio::with_cleanup(
            [&]() -> task<void> {
              events.push_back(2);
              co_return;
            },
            [&](const faio::task_exit& exit) -> task<void> {
              EXPECT_EQ(exit.status, faio::task_exit::kind::succeeded);
              events.push_back(3);
              co_await faio::this_coro::yield();
              events.push_back(4);
            });
        events.push_back(5);
      },
      [&](const faio::task_exit& exit) -> task<void> {
        EXPECT_EQ(exit.status, faio::task_exit::kind::succeeded);
        events.push_back(6);
        co_await faio::this_coro::yield();
        events.push_back(7);
      });
  runtime.block_on(std::move(wrapped));
  EXPECT_EQ(events, (std::vector{1, 2, 3, 4, 5, 6, 7}));
}

/** @brief 记录已接纳分支的实际执行次数。 */
task<void> mark_restricted_child(int& runs) {
  ++runs;
  co_return;
}

/** @brief 填满私有队列，测试 join_all 部分接纳失败后是否排空任务。 */
task<void> saturate_private_queue(std::array<int, 6>& runs) {
  std::vector<task<void>> branches;
  branches.reserve(runs.size());
  for (auto& counter : runs)
    branches.push_back(mark_restricted_child(counter));
  co_await faio::join_all(std::move(branches));
}

TEST(CoroutineCleanup, RestrictedQueueFailureDrainsAndStillRunsCleanup) {
  current_thread_runtime runtime{faio::config_builder{}.set_no_stealing_queue_capacity(4).build()};
  std::array<int, 6> runs{};
  bool cleanup_finished = false;
  auto wrapped =
      faio::with_cleanup([&] { return saturate_private_queue(runs); },
                         [&](const faio::task_exit& exit) -> task<void> {
                           EXPECT_EQ(exit.status, faio::task_exit::kind::failed);
                           EXPECT_EQ(runs, (std::array{1, 1, 1, 0, 0, 0}));
                           EXPECT_TRUE(detail::current_task_context().scheduling.no_stealing);
                           co_await faio::this_coro::yield();
                           EXPECT_TRUE(detail::current_task_context().scheduling.no_stealing);
                           cleanup_finished = true;
                         });
  EXPECT_THROW(runtime.block_on(std::move(wrapped).with_no_stealing()), std::overflow_error);
  EXPECT_TRUE(cleanup_finished);
  EXPECT_EQ(runs, (std::array{1, 1, 1, 0, 0, 0}));
}

/** @brief 等待测试允许交付，再记录实际完成次序。 */
task<std::unique_ptr<int>> group_value(faio::sync::latch& gate,
                                       std::vector<int>& order,
                                       int value) {
  co_await gate.wait();
  order.push_back(value);
  co_return std::make_unique<int>(value);
}

/** @brief 关闭接纳后校验特定错误码，被拒绝的任务不能实际启动。 */
void expect_closed_group(faio::task_group_context& context, int& starts) {
  try {
    (void)context.spawn_observed(observed_value(starts));
    ADD_FAILURE() << "closed group accepted a new child";
  } catch (const std::system_error& failure) {
    EXPECT_EQ(failure.code(), faio::make_error_code(faio::task_group_errc::closed));
  }
}

TEST(CoroutineGroup, ResultsFollowAdmissionOrderAndHandlesAreOneShot) {
  current_thread_runtime runtime;
  faio::sync::latch first_gate{1};
  faio::sync::latch second_gate{1};
  std::vector<int> execution_order;
  int rejected_starts = 0;
  auto result = runtime.block_on(faio::task_group(
      faio::group_options{faio::group_error_policy::collect_all},
      [&](faio::task_group_context& context) -> task<int> {
        auto first = context.spawn_observed(group_value(first_gate, execution_order, 1));
        auto second = context.spawn_observed(group_value(second_gate, execution_order, 2));
        context.close();
        context.close();
        expect_closed_group(context, rejected_starts);

        second_gate.count_down();
        auto second_result = co_await second;
        const auto second_value = second_result.take_value();
        EXPECT_EQ(second_value ? *second_value : -1, 2);
        bool duplicate_rejected = false;
        try {
          (void)co_await std::move(second);
        } catch (const std::logic_error&) {
          duplicate_rejected = true;
        }
        EXPECT_TRUE(duplicate_rejected);

        first_gate.count_down();
        auto first_result = co_await std::move(first);
        const auto first_value = first_result.take_value();
        EXPECT_EQ(first_value ? *first_value : -1, 1);
        co_return 41;
      }));
  EXPECT_EQ(result.body.take_value(), 41);
  EXPECT_EQ(execution_order, (std::vector{2, 1}));
  EXPECT_EQ(rejected_starts, 0);
  ASSERT_EQ(result.children.size(), 2U);
  for (std::size_t index = 0; index < result.children.size(); ++index) {
    EXPECT_EQ(result.children[index].index, index);
    EXPECT_EQ(result.children[index].status, status::value);
    EXPECT_EQ(result.children[index].exception, nullptr);
  }
}

TEST(CoroutineGroup, ConcurrentCloseAndAdmissionEitherAcceptsOrRejectsExactlyOnce) {
  current_thread_runtime runtime;
  for (int attempt = 0; attempt < 12; ++attempt) {
    SCOPED_TRACE(attempt);
    int starts = 0;
    bool accepted = false;
    bool rejected = false;
    auto result = runtime.block_on(faio::task_group(
        faio::group_options{}, [&](faio::task_group_context& context) -> task<void> {
          std::optional<faio::scoped_join_handle<int>> child;
          {
            std::latch start{2};
            const std::jthread closer{[&] {
              start.arrive_and_wait();
              context.close();
            }};
            start.arrive_and_wait();
            try {
              child.emplace(context.spawn_observed(observed_value(starts)));
              accepted = true;
            } catch (const std::system_error& failure) {
              EXPECT_EQ(failure.code(), faio::make_error_code(faio::task_group_errc::closed));
              rejected = true;
            }
          }
          if (child) {
            auto outcome = co_await *child;
            EXPECT_EQ(outcome.take_value(), 42);
          }
        }));

    EXPECT_NE(accepted, rejected);
    EXPECT_EQ(result.body.status(), status::value);
    ASSERT_EQ(result.children.size(), accepted ? 1U : 0U);
    EXPECT_EQ(starts, accepted ? 1 : 0);
    if (accepted) {
      EXPECT_EQ(result.children.front().index, 0U);
      EXPECT_EQ(result.children.front().status, status::value);
    }
  }
}

/** @brief 在另一词法任务中尝试领取已离开主体作用域的句柄。 */
task<void> await_escaped_group_handle(faio::scoped_join_handle<int>& handle) {
  (void)co_await handle;
}

TEST(CoroutineGroup, EscapedHandleCannotBeConsumedAfterBodyExits) {
  current_thread_runtime runtime;
  std::optional<faio::scoped_join_handle<int>> escaped;
  int starts = 0;
  auto result = runtime.block_on(
      faio::task_group(faio::group_options{}, [&](faio::task_group_context& context) -> task<void> {
        escaped.emplace(context.spawn_observed(observed_value(starts)));
        co_return;
      }));

  EXPECT_EQ(starts, 1);
  EXPECT_EQ(result.body.status(), status::value);
  ASSERT_EQ(result.children.size(), 1U);
  EXPECT_EQ(result.children.front().status, status::value);
  ASSERT_TRUE(escaped.has_value());
  EXPECT_THROW(runtime.block_on(await_escaped_group_handle(*escaped)), std::logic_error);
}

TEST(CoroutineGroup, CollectAllKeepsSiblingsRunningAndRetainsEveryError) {
  current_thread_runtime runtime;
  auto first_failure = std::make_exception_ptr(8);
  auto second_failure = std::make_exception_ptr(std::runtime_error("second child"));
  auto body_failure = std::make_exception_ptr(std::runtime_error("group body"));
  std::stop_token token;
  bool sibling_ran = false;
  auto result = runtime.block_on(
      faio::task_group(faio::group_options{faio::group_error_policy::collect_all},
                       [&](faio::task_group_context& context) -> task<void> {
                         token = context.stop_token();
                         auto first = context.spawn_observed(rethrow_task(first_failure));
                         auto second = context.spawn_observed(rethrow_task(second_failure));
                         (void)context.spawn_observed([](bool& ran) -> task<void> {
                           EXPECT_FALSE((co_await faio::this_coro::stop_token()).stop_requested());
                           ran = true;
                         }(sibling_ran));
                         const auto first_result = co_await first;
                         EXPECT_EQ(first_result.exception(), first_failure);
                         const auto second_result = co_await second;
                         EXPECT_EQ(second_result.exception(), second_failure);
                         EXPECT_FALSE(token.stop_requested());
                         std::rethrow_exception(body_failure);
                       }));
  EXPECT_TRUE(sibling_ran);
  EXPECT_FALSE(token.stop_requested());
  EXPECT_EQ(result.body.status(), status::error);
  EXPECT_EQ(result.body.exception(), body_failure);
  ASSERT_EQ(result.children.size(), 3U);
  EXPECT_EQ(result.children[0].status, status::error);
  EXPECT_EQ(result.children[0].exception, first_failure);
  EXPECT_EQ(result.children[1].status, status::error);
  EXPECT_EQ(result.children[1].exception, second_failure);
  EXPECT_EQ(result.children[2].status, status::value);
}

TEST(CoroutineGroup, DroppedHandleFailureIsObservedAndDoesNotEscape) {
  current_thread_runtime runtime;
  const auto ignored = std::make_exception_ptr(17);
  auto result =
      runtime.block_on(faio::task_group(faio::group_options{faio::group_error_policy::collect_all},
                                        [&](faio::task_group_context& context) -> task<void> {
                                          (void)context.spawn_observed(rethrow_task(ignored));
                                          co_return;
                                        }));

  EXPECT_EQ(result.body.status(), status::value);
  ASSERT_EQ(result.children.size(), 1U);
  EXPECT_EQ(result.children.front().index, 0U);
  EXPECT_EQ(result.children.front().status, status::error);
  EXPECT_EQ(result.children.front().exception, ignored);
}

/** @brief 失败时先请求同组任务停止，等清理完成并结束资源借用后，任务组才算完成。 */
task<void> check_group_failure_drains() {
  faio::sync::latch peer_entered{1};
  faio::sync::latch peer_gate{1};
  faio::sync::latch cleanup_entered{1};
  faio::sync::latch cleanup_gate{1};
  const auto sentinel = std::make_exception_ptr(67);
  std::stop_token group_token;
  std::stop_token peer_token;
  std::exception_ptr peer_cancel;
  bool cleaned = false;
  auto completion = faio::spawn(
      faio::task_group(faio::group_options{}, [&](faio::task_group_context& context) -> task<void> {
        group_token = context.stop_token();
        (void)context.spawn_observed(faio::with_cleanup(
            [&] { return wait_until_cancelled(peer_entered, peer_gate, peer_token); },
            [&](const faio::task_exit& exit) -> task<void> {
              peer_cancel = exit.exception;
              EXPECT_EQ(exit.status, faio::task_exit::kind::cancelled);
              cleanup_entered.count_down();
              co_await cleanup_gate.wait();
              cleaned = true;
            }));
        co_await peer_entered.wait();
        auto failure = context.spawn_observed(rethrow_task(sentinel));
        const auto observed = co_await failure;
        EXPECT_EQ(observed.status(), status::error);
        EXPECT_EQ(observed.exception(), sentinel);
        EXPECT_TRUE(group_token.stop_requested());
      }));

  co_await cleanup_entered.wait();
  EXPECT_TRUE(group_token.stop_requested());
  EXPECT_TRUE(peer_token.stop_requested());
  EXPECT_FALSE(cleaned);
  EXPECT_FALSE(completion.done());
  cleanup_gate.count_down();

  auto result = co_await completion;
  EXPECT_TRUE(cleaned);
  EXPECT_EQ(result.body.status(), status::value);
  EXPECT_NE(peer_cancel, nullptr);
  EXPECT_EQ(result.children.size(), 2U);
  if (result.children.size() == 2U) {
    EXPECT_EQ(result.children[0].status, status::stopped);
    EXPECT_EQ(result.children[0].exception, peer_cancel);
    EXPECT_EQ(result.children[1].status, status::error);
    EXPECT_EQ(result.children[1].exception, sentinel);
  }
}

TEST(CoroutineGroup, FailFastStopsSiblingsButWaitsForTheirCleanup) {
  current_thread_runtime runtime;
  runtime.block_on(check_group_failure_drains());
}

/** @brief 分别观察停止请求和关闭操作，确认已接纳的任务全部退出后才返回组结果。 */
task<void> check_group_close_and_parent_cancel() {
  faio::sync::latch child_entered{1};
  faio::detail::completion_event child_gate;
  faio::sync::latch body_closed{1};
  std::stop_token group_token;
  int rejected_starts = 0;
  auto completion = faio::spawn(faio::task_group(
      faio::group_options{faio::group_error_policy::collect_all},
      [&](faio::task_group_context& context) -> task<void> {
        group_token = context.stop_token();
        auto accepted = context.spawn_observed(return_after_stop(child_entered, child_gate));
        co_await child_entered.wait();
        context.close();
        expect_closed_group(context, rejected_starts);
        body_closed.count_down();
        auto result = co_await accepted;
        EXPECT_EQ(result.take_value(), 19);
      }));
  co_await body_closed.wait();
  EXPECT_TRUE(group_token.stop_possible());
  EXPECT_FALSE(group_token.stop_requested());
  EXPECT_FALSE(completion.done());
  completion.request_stop();
  EXPECT_TRUE(group_token.stop_requested());
  EXPECT_FALSE(completion.done());
  child_gate.notify();
  auto result = co_await completion;
  EXPECT_EQ(result.body.status(), status::value);
  EXPECT_EQ(rejected_starts, 0);
  EXPECT_EQ(result.children.size(), 1U);
  if (result.children.size() == 1U)
    EXPECT_EQ(result.children.front().status, status::value);
}

TEST(CoroutineGroup, ParentStopDoesNotConflateCloseWithDrain) {
  current_thread_runtime runtime;
  runtime.block_on(check_group_close_and_parent_cancel());
}

TEST(CoroutineGroup, NestedGroupsDrainLocallyWithoutUpstreamStop) {
  current_thread_runtime runtime;
  int inner_calls = 0;
  auto outer = runtime.block_on(faio::task_group(
      faio::group_options{}, [&](faio::task_group_context& outer_context) -> task<void> {
        const auto outer_token = outer_context.stop_token();
        auto nested = co_await faio::task_group(
            faio::group_options{}, [&](faio::task_group_context& inner_context) -> task<int> {
              inner_context.request_stop();
              EXPECT_TRUE(inner_context.stop_token().stop_requested());
              (void)inner_context.spawn_observed(observed_value(inner_calls));
              co_return 32;
            });
        EXPECT_EQ(nested.body.take_value(), 32);
        EXPECT_EQ(nested.children.size(), 1U);
        EXPECT_FALSE(outer_token.stop_requested());
      }));
  EXPECT_EQ(inner_calls, 1);
  EXPECT_EQ(outer.body.status(), status::value);
  EXPECT_TRUE(outer.children.empty());
}

TEST(CoroutineGroup, RestrictedAdmissionFailureRollsBackAndLeavesQueueReusable) {
  current_thread_runtime runtime{faio::config_builder{}.set_no_stealing_queue_capacity(4).build()};
  std::array<int, 6> runs{};
  auto result = runtime.block_on(
      faio::task_group(faio::group_options{faio::group_error_policy::collect_all},
                       [&](faio::task_group_context& context) -> task<void> {
                         for (auto& count : runs)
                           (void)context.spawn_observed(mark_restricted_child(count));
                         co_return;
                       })
          .with_no_stealing());

  EXPECT_EQ(result.body.status(), status::error);
  EXPECT_THROW(result.body.value_or_throw(), std::overflow_error);
  EXPECT_GT(result.children.size(), 0U);
  EXPECT_LT(result.children.size(), runs.size());
  for (std::size_t i = 0; i < runs.size(); ++i) {
    EXPECT_EQ(runs[i], i < result.children.size() ? 1 : 0);
    if (i < result.children.size()) {
      EXPECT_EQ(result.children[i].index, i);
      EXPECT_EQ(result.children[i].status, status::value);
    }
  }

  auto subsequent = runtime.block_on(
      faio::task_group(faio::group_options{}, [&](faio::task_group_context& context) -> task<void> {
        (void)context.spawn_observed(mark_restricted_child(runs.back()));
        co_return;
      }).with_no_stealing());
  EXPECT_EQ(subsequent.body.status(), status::value);
  EXPECT_EQ(subsequent.children.size(), 1U);
  EXPECT_EQ(runs.back(), 1);
}

/** @brief 记录输入任务的启动、退出和并发数。 */
struct window_observation {
  faio::sync::latch first_window{2};
  std::array<faio::detail::completion_event, 4> gates;
  std::array<unsigned, 4> starts{};
  std::array<std::stop_token, 4> tokens;
  unsigned active{};
  unsigned peak{};
  unsigned started{};
};

/** @brief 等待测试放行后才释放并发名额。 */
task<int> hold_limited_window(window_observation& window, std::size_t index) {
  window.tokens[index] = co_await faio::this_coro::stop_token();
  ++window.starts[index];
  ++window.active;
  window.peak = std::max(window.peak, window.active);
  if (++window.started <= 2)
    window.first_window.count_down();
  co_await window.gates[index].wait();
  --window.active;
  co_return static_cast<int>(index + 10);
}

/** @brief 让窗口在两个已运行的任务均未退出时接收停止请求。 */
task<void> check_limited_window(bool stop_requested) {
  window_observation window;
  std::vector<task<int>> jobs;
  for (std::size_t i = 0; i < window.gates.size(); ++i)
    jobs.push_back(hold_limited_window(window, i));
  auto completion = faio::spawn(faio::join_all_limited(2, std::move(jobs)));

  co_await window.first_window.wait();
  EXPECT_EQ(window.active, 2U);
  EXPECT_EQ(window.peak, 2U);
  EXPECT_EQ(window.starts, (std::array{1U, 1U, 0U, 0U}));
  EXPECT_FALSE(completion.done());
  if (stop_requested) {
    completion.request_stop();
    EXPECT_TRUE(window.tokens[0].stop_requested());
    EXPECT_TRUE(window.tokens[1].stop_requested());
    EXPECT_FALSE(completion.done());
  }
  for (auto& gate : window.gates)
    gate.notify();
  auto outcomes = co_await completion;
  EXPECT_EQ(window.active, 0U);
  EXPECT_EQ(window.peak, 2U);
  EXPECT_EQ(outcomes.size(), window.gates.size());
  for (std::size_t i = 0; i < outcomes.size(); ++i) {
    if (stop_requested && i >= 2) {
      EXPECT_EQ(outcomes[i].status(), status::stopped);
      EXPECT_EQ(outcomes[i].stop_reason(), faio::task_stop_reason::not_started);
      EXPECT_EQ(window.starts[i], 0U);
    } else {
      EXPECT_EQ(outcomes[i].status(), status::value);
      EXPECT_EQ(outcomes[i].take_value(), static_cast<int>(i + 10));
      EXPECT_EQ(window.starts[i], 1U);
    }
  }
}

TEST(CoroutineGroup, LimitedJoinHonorsCapacityAndKeepsInputOrder) {
  current_thread_runtime runtime;
  EXPECT_THROW((void)faio::join_all_limited(0, std::vector<task<void>>{}), std::invalid_argument);
  auto empty = runtime.block_on(faio::join_all_limited(2, std::vector<task<void>>{}));
  EXPECT_TRUE(empty.empty());
  runtime.block_on(check_limited_window(false));
}

TEST(CoroutineGroup, RestrictedLimitedJoinDrainsAfterOversizedWindowAdmissionFails) {
  current_thread_runtime runtime{faio::config_builder{}.set_no_stealing_queue_capacity(4).build()};
  std::array<int, 6> starts{};
  std::vector<task<int>> jobs;
  for (auto& counter : starts)
    jobs.push_back(observed_value(counter));

  auto outcomes = runtime.block_on(faio::join_all_limited(6, std::move(jobs)).with_no_stealing());
  ASSERT_EQ(outcomes.size(), starts.size());
  std::size_t failure_position = 0;
  while (failure_position < outcomes.size()
         && outcomes[failure_position].status() == status::value) {
    EXPECT_EQ(outcomes[failure_position].take_value(), 42);
    EXPECT_EQ(starts[failure_position], 1);
    ++failure_position;
  }
  ASSERT_GT(failure_position, 0U);
  ASSERT_LT(failure_position, outcomes.size());
  EXPECT_EQ(outcomes[failure_position].status(), status::error);
  EXPECT_THROW((void)outcomes[failure_position].value_or_throw(), std::overflow_error);
  for (std::size_t index = failure_position; index < outcomes.size(); ++index) {
    EXPECT_EQ(starts[index], 0);
    if (index != failure_position) {
      EXPECT_EQ(outcomes[index].status(), status::stopped);
      EXPECT_EQ(outcomes[index].stop_reason(), faio::task_stop_reason::not_started);
    }
  }

  int subsequent_starts = 0;
  std::vector<task<int>> subsequent_jobs;
  subsequent_jobs.push_back(observed_value(subsequent_starts));
  auto subsequent =
      runtime.block_on(faio::join_all_limited(1, std::move(subsequent_jobs)).with_no_stealing());
  ASSERT_EQ(subsequent.size(), 1U);
  EXPECT_EQ(subsequent.front().take_value(), 42);
  EXPECT_EQ(subsequent_starts, 1);
}

TEST(CoroutineGroup, LimitedJoinMarksUnstartedChildrenAfterParentStop) {
  current_thread_runtime runtime;
  runtime.block_on(check_limited_window(true));
}

/** @brief 在窗口内让一个分支实际挂起后，再触发同组分支失败。 */
task<void> fail_after_entry(faio::sync::latch& entered, std::exception_ptr failure) {
  co_await entered.wait();
  std::rethrow_exception(failure);
}

TEST(CoroutineGroup, LimitedFailFastPartitionsFailureCancellationAndNotStarted) {
  current_thread_runtime runtime;
  faio::sync::latch entered{1};
  faio::sync::latch gate{1};
  std::stop_token peer_token;
  int queued_starts = 0;
  const auto failure = std::make_exception_ptr(109);
  std::vector<task<void>> jobs;
  jobs.push_back(fail_after_entry(entered, failure));
  jobs.push_back(wait_until_cancelled(entered, gate, peer_token));
  jobs.push_back([](int& starts) -> task<void> {
    ++starts;
    co_return;
  }(queued_starts));

  const auto outcomes = runtime.block_on(faio::join_all_limited(2, std::move(jobs)));
  ASSERT_EQ(outcomes.size(), 3U);
  EXPECT_EQ(outcomes[0].status(), status::error);
  EXPECT_EQ(outcomes[0].exception(), failure);
  EXPECT_EQ(outcomes[1].status(), status::stopped);
  EXPECT_EQ(outcomes[1].stop_reason(), faio::task_stop_reason::requested);
  EXPECT_TRUE(peer_token.stop_requested());
  EXPECT_EQ(outcomes[2].status(), status::stopped);
  EXPECT_EQ(outcomes[2].stop_reason(), faio::task_stop_reason::not_started);
  EXPECT_EQ(queued_starts, 0);
}

/** @brief 保留原异常对象，也允许动态组合使用相同的任务类型。 */
task<int> rethrow_int_task(std::exception_ptr failure) {
  std::rethrow_exception(failure);
  co_return 0;
}

TEST(CoroutineSettled, HeterogeneousJoinKeepsRepeatedTypesVoidAndMoveOnlyValues) {
  current_thread_runtime runtime;
  int starts = 0;
  auto result = runtime.block_on(faio::join_settled(
      observed_value(starts), returns_void(), unique_value(), observed_value(starts)));
  static_assert(std::same_as<decltype(result),
                             std::tuple<task_outcome<int>,
                                        task_outcome<void>,
                                        task_outcome<std::unique_ptr<int>>,
                                        task_outcome<int>>>);
  EXPECT_EQ(starts, 2);
  EXPECT_EQ(std::get<0>(result).take_value(), 42);
  EXPECT_NO_THROW(std::get<1>(result).take_value());
  auto value = std::get<2>(result).take_value();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 17);
  EXPECT_EQ(std::get<3>(result).take_value(), 42);
}

/** @brief 正常返回前记录两个时点的令牌和实际完成顺序。 */
task<int> hold_settled_value(faio::detail::completion_event& gate,
                             faio::sync::latch& entered,
                             faio::detail::completion_event& exited,
                             std::stop_token& token,
                             int value) {
  token = co_await faio::this_coro::stop_token();
  entered.count_down();
  co_await gate.wait();
  exited.notify();
  co_return value;
}

/** @brief 较慢输入始终保持未完成，直到同组失败和后序成功已经发生。 */
task<void> check_settled_order_and_independent_failure() {
  faio::detail::completion_event first_gate;
  faio::detail::completion_event last_gate;
  faio::detail::completion_event first_exit;
  faio::detail::completion_event last_exit;
  faio::sync::latch entered{2};
  std::stop_token first_token;
  std::stop_token last_token;
  const auto failure = std::make_exception_ptr(515);
  std::vector<task<int>> jobs;
  jobs.push_back(hold_settled_value(first_gate, entered, first_exit, first_token, 10));
  jobs.push_back(rethrow_int_task(failure));
  jobs.push_back(hold_settled_value(last_gate, entered, last_exit, last_token, 30));
  auto operation = faio::spawn(faio::join_all_settled(std::move(jobs)));

  co_await entered.wait();
  last_gate.notify();
  co_await last_exit.wait();
  EXPECT_FALSE(first_token.stop_requested());
  EXPECT_FALSE(last_token.stop_requested());
  EXPECT_FALSE(operation.done());
  first_gate.notify();

  auto outcomes = co_await operation;
  EXPECT_TRUE(first_exit.wait().await_ready());
  EXPECT_EQ(outcomes.size(), 3U);
  if (outcomes.size() == 3U) {
    EXPECT_EQ(outcomes[0].take_value(), 10);
    EXPECT_EQ(outcomes[1].status(), status::error);
    EXPECT_EQ(outcomes[1].exception(), failure);
    EXPECT_EQ(outcomes[2].take_value(), 30);
  }
}

TEST(CoroutineSettled, DynamicJoinKeepsInputOrderAndDoesNotStopSiblingsOnError) {
  current_thread_runtime runtime;
  runtime.block_on(check_settled_order_and_independent_failure());
  const auto empty = runtime.block_on(faio::join_all_settled(std::vector<task<int>>{}));
  EXPECT_TRUE(empty.empty());
}

TEST(CoroutineSettled, ProducerMoveFailureIsLocalAndConsumerMoveDoesNotRewriteValue) {
  current_thread_runtime runtime;
  move_control failed{.throw_on = 2};
  move_control consumed{.throw_on = 3};
  auto outcomes = runtime.block_on(faio::join_settled(result_move(failed), result_move(consumed)));
  auto& failure = std::get<0>(outcomes);
  auto& success = std::get<1>(outcomes);
  EXPECT_EQ(failure.status(), status::error);
  EXPECT_THROW((void)failure.value_or_throw(), std::runtime_error);
  EXPECT_EQ(success.status(), status::value);
  EXPECT_THROW((void)success.take_value(), std::runtime_error);
  EXPECT_EQ(success.status(), status::value);
  EXPECT_THROW((void)success.take_value(), std::logic_error);
}

struct partial_branch_observation {
  int starts{};
  int exits{};
  bool stopped{};
};

/** @brief 在受限执行器上实际让出一次，验证已接纳的工作全部退出后才交付结果。 */
task<int> observe_partial_branch(partial_branch_observation& observation) {
  ++observation.starts;
  const auto token = co_await faio::this_coro::stop_token();
  co_await faio::this_coro::yield();
  observation.stopped = token.stop_requested();
  ++observation.exits;
  co_return 85;
}

enum class partial_stop { preserve, request };

/** @brief 记录实际接纳的输入前缀，确认失败位置之后的惰性任务均未启动。 */
void expect_partial_exit(const std::array<partial_branch_observation, 8>& branches,
                         partial_stop stop) {
  std::size_t accepted = 0;
  while (accepted < branches.size() && branches[accepted].starts != 0) {
    EXPECT_EQ(branches[accepted].starts, 1);
    EXPECT_EQ(branches[accepted].exits, 1);
    EXPECT_EQ(branches[accepted].stopped, stop == partial_stop::request);
    ++accepted;
  }
  EXPECT_GT(accepted, 0U);
  EXPECT_LT(accepted, branches.size());
  for (std::size_t index = accepted; index < branches.size(); ++index) {
    EXPECT_EQ(branches[index].starts, 0);
    EXPECT_EQ(branches[index].exits, 0);
  }
}

TEST(CoroutineSettled, PartialAdmissionFailureDrainsAcceptedBranchesBeforeThrowing) {
  current_thread_runtime runtime{faio::config_builder{}.set_no_stealing_queue_capacity(4).build()};
  std::array<partial_branch_observation, 8> branches{};
  std::vector<task<int>> jobs;
  for (auto& branch : branches)
    jobs.push_back(observe_partial_branch(branch));

  auto join = faio::join_all_settled(std::move(jobs)).with_no_stealing();
  EXPECT_THROW((void)runtime.block_on(std::move(join)), std::overflow_error);
  expect_partial_exit(branches, partial_stop::preserve);

  std::vector<task<void>> subsequent_jobs;
  subsequent_jobs.push_back(returns_void());
  auto subsequent =
      runtime.block_on(faio::join_all_settled(std::move(subsequent_jobs)).with_no_stealing());
  ASSERT_EQ(subsequent.size(), 1U);
  EXPECT_EQ(subsequent.front().status(), status::value);
}

TEST(CoroutineSelectSuccess, ErrorThenSuccessUsesTheRepeatedVariantIndex) {
  current_thread_runtime runtime;
  int starts = 0;
  const auto failure = std::make_exception_ptr(129);
  const auto result =
      runtime.block_on(faio::select_success(rethrow_int_task(failure), observed_value(starts)));
  EXPECT_EQ(starts, 1);
  EXPECT_EQ(result.winner_index, 1U);
  EXPECT_TRUE(result.failures.empty());
  ASSERT_TRUE(result.value);
  ASSERT_EQ(result.value->index(), 1U);
  EXPECT_EQ(std::get<1>(*result.value), 42);
}

TEST(CoroutineSelectSuccess, VoidAndMoveOnlyWinnersAreRepresented) {
  current_thread_runtime runtime;
  const auto void_winner =
      runtime.block_on(faio::select_success(throws_standard(), returns_void()));
  EXPECT_EQ(void_winner.winner_index, 1U);
  ASSERT_TRUE(void_winner.value);
  EXPECT_EQ(void_winner.value->index(), 1U);
  EXPECT_TRUE(void_winner.failures.empty());

  const auto move_winner =
      runtime.block_on(faio::select_success(throws_nonstandard(), unique_value()));
  EXPECT_EQ(move_winner.winner_index, 1U);
  ASSERT_TRUE(move_winner.value);
  ASSERT_EQ(move_winner.value->index(), 1U);
  ASSERT_TRUE(std::get<1>(*move_winner.value));
  EXPECT_EQ(*std::get<1>(*move_winner.value), 17);
}

TEST(CoroutineSelectSuccess, SlotMoveFailureCannotClaimWinnerAndFinalMoveFailureEscapes) {
  current_thread_runtime runtime;
  move_control first_failed{.throw_on = 2};
  move_control second_moved;
  auto result =
      runtime.block_on(faio::select_success(result_move(first_failed), result_move(second_moved)));
  EXPECT_EQ(result.winner_index, 1U);
  ASSERT_TRUE(result.value);
  EXPECT_EQ(result.value->index(), 1U);
  EXPECT_EQ(first_failed.moves, 2);
  ASSERT_GE(second_moved.moves, 3);

  move_control final_failed{.throw_on = 3};
  EXPECT_THROW((void)runtime.block_on(faio::select_success(result_move(final_failed))),
               std::runtime_error);
}

task<int> return_after_competition_gate(faio::detail::completion_event& gate,
                                        faio::sync::latch& entered,
                                        int value,
                                        int& success_count) {
  entered.count_down();
  co_await gate.wait();
  ++success_count;
  co_return value;
}

/** @brief 放行两个竞争成功结果的分支；已收到停止请求的落选分支仍需等待清理门闩打开。 */
task<void> check_select_success_race_and_cleanup() {
  faio::detail::completion_event racing_gate;
  faio::sync::latch racing_entered{2};
  faio::sync::latch loser_entered{1};
  faio::sync::latch loser_gate{1};
  faio::sync::latch cleanup_entered{1};
  faio::sync::latch cleanup_gate{1};
  std::stop_token loser_token;
  int successful = 0;
  int cleanup_calls = 0;
  bool cleanup_finished = false;
  auto rival = faio::with_cleanup(
      [&] { return wait_until_cancelled(loser_entered, loser_gate, loser_token); },
      [&](const faio::task_exit& exit) -> task<void> {
        ++cleanup_calls;
        EXPECT_EQ(exit.status, faio::task_exit::kind::cancelled);
        EXPECT_NE(exit.exception, nullptr);
        cleanup_entered.count_down();
        co_await cleanup_gate.wait();
        cleanup_finished = true;
      });
  auto race = faio::spawn(faio::select_success(
      std::move(rival),
      return_after_competition_gate(racing_gate, racing_entered, 31, successful),
      return_after_competition_gate(racing_gate, racing_entered, 47, successful)));

  co_await racing_entered.wait();
  co_await loser_entered.wait();
  EXPECT_FALSE(loser_token.stop_requested());
  EXPECT_FALSE(race.done());
  racing_gate.notify();

  co_await cleanup_entered.wait();
  EXPECT_TRUE(loser_token.stop_requested());
  EXPECT_EQ(cleanup_calls, 1);
  EXPECT_FALSE(cleanup_finished);
  EXPECT_FALSE(race.done());
  cleanup_gate.count_down();

  auto result = co_await race;
  EXPECT_TRUE(cleanup_finished);
  EXPECT_EQ(successful, 2);
  EXPECT_TRUE(result.failures.empty());
  EXPECT_TRUE(result.winner_index == 1U || result.winner_index == 2U);
  EXPECT_TRUE(result.value);
  if (result.winner_index && result.value) {
    EXPECT_EQ(result.value->index(), *result.winner_index);
    if (result.value->index() == 1U)
      EXPECT_EQ(std::get<1>(*result.value), 31);
    if (result.value->index() == 2U)
      EXPECT_EQ(std::get<2>(*result.value), 47);
  }
}

TEST(CoroutineSelectSuccess, SimultaneousSuccessfulBranchesClaimOneWinnerAndDrainLateCleanup) {
  current_thread_runtime runtime;
  runtime.block_on(check_select_success_race_and_cleanup());
}

task<void> fail_after_gate(faio::detail::completion_event& gate, std::exception_ptr failure) {
  co_await gate.wait();
  std::rethrow_exception(failure);
}

TEST(CoroutineSelectSuccess, AllErrorsAreInInputOrderDespiteOutOfOrderCompletion) {
  current_thread_runtime runtime;
  faio::detail::completion_event gate;
  const auto slow = std::make_exception_ptr(31);
  const auto fast = std::make_exception_ptr(29);
  const auto release_then_fail = [&]() -> task<void> {
    gate.notify();
    std::rethrow_exception(fast);
    co_return;
  };
  const auto result =
      runtime.block_on(faio::select_success(fail_after_gate(gate, slow), release_then_fail()));
  EXPECT_FALSE(result.winner_index);
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.failures.size(), 2U);
  for (std::size_t index = 0; index < result.failures.size(); ++index) {
    EXPECT_EQ(result.failures[index].index, index);
    EXPECT_EQ(result.failures[index].status, status::error);
    EXPECT_FALSE(result.failures[index].stop_reason);
  }
  EXPECT_EQ(result.failures[0].exception, slow);
  EXPECT_EQ(result.failures[1].exception, fast);
}

/** @brief 确认父任务的停止请求传给两个等待中的子任务，并按输入下标检查停止元数据。 */
task<void> check_all_stopped_select() {
  faio::sync::latch entered{2};
  faio::sync::latch never_released{1};
  std::array<std::stop_token, 2> tokens{};
  auto race =
      faio::spawn(faio::select_success(wait_until_cancelled(entered, never_released, tokens[0]),
                                       wait_until_cancelled(entered, never_released, tokens[1])));

  co_await entered.wait();
  EXPECT_FALSE(race.done());
  race.request_stop();
  auto result = co_await race;
  EXPECT_FALSE(result.winner_index);
  EXPECT_FALSE(result.value);
  EXPECT_EQ(result.failures.size(), tokens.size());
  for (std::size_t index = 0; index < result.failures.size(); ++index) {
    EXPECT_EQ(result.failures[index].index, index);
    EXPECT_EQ(result.failures[index].status, status::stopped);
    EXPECT_EQ(result.failures[index].stop_reason, faio::task_stop_reason::requested);
    EXPECT_NE(result.failures[index].exception, nullptr);
  }
  for (const auto& token : tokens)
    EXPECT_TRUE(token.stop_requested());
}

TEST(CoroutineSelectSuccess, AllStoppedBranchesKeepTheirInputPositionsAndReasons) {
  current_thread_runtime runtime;
  runtime.block_on(check_all_stopped_select());
}

/** @brief 停止请求只传播令牌，继续完成的成功分支仍可以获胜。 */
task<void> check_parent_stop_and_late_success() {
  faio::sync::latch entered{2};
  faio::sync::latch unrelated_gate{1};
  faio::detail::completion_event value_gate;
  std::stop_token cancelled_token;
  auto operation = faio::spawn(
      faio::select_success(wait_until_cancelled(entered, unrelated_gate, cancelled_token),
                           return_after_stop(entered, value_gate)));

  co_await entered.wait();
  operation.request_stop();
  EXPECT_TRUE(cancelled_token.stop_requested());
  EXPECT_FALSE(operation.done());
  value_gate.notify();

  const auto result = co_await operation;
  EXPECT_EQ(result.winner_index, 1U);
  EXPECT_TRUE(result.failures.empty());
  EXPECT_TRUE(result.value);
  if (result.value && result.value->index() == 1U)
    EXPECT_EQ(std::get<1>(*result.value), 19);
  else
    ADD_FAILURE() << "the branch that returned a value did not win";
}

TEST(CoroutineSelectSuccess, ParentStopDoesNotPreventAnActualSuccessFromWinning) {
  current_thread_runtime runtime;
  runtime.block_on(check_parent_stop_and_late_success());
}

template <std::size_t... Index>
auto select_partial_branches(std::array<partial_branch_observation, sizeof...(Index)>& branches,
                             std::index_sequence<Index...>) {
  return faio::select_success(observe_partial_branch(branches[Index])...);
}

TEST(CoroutineSelectSuccess, PartialAdmissionFailureStopsAndDrainsAcceptedBranches) {
  current_thread_runtime runtime{faio::config_builder{}.set_no_stealing_queue_capacity(4).build()};
  std::array<partial_branch_observation, 8> branches{};
  auto race = select_partial_branches(branches, std::make_index_sequence<branches.size()>{})
                  .with_no_stealing();
  EXPECT_THROW((void)runtime.block_on(std::move(race)), std::overflow_error);
  expect_partial_exit(branches, partial_stop::request);
  auto subsequent = runtime.block_on(faio::select_success(returns_void()).with_no_stealing());
  EXPECT_EQ(subsequent.winner_index, 0U);
}
}  // namespace
