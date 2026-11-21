/**
 * @file faio/detail/coroutine/composition/scope.hpp
 * @brief 结构化任务组视图、子任务包装与排空实现。
 */
#pragma once

#include "faio/detail/coroutine/launch/observed_task.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/coroutine/composition/scope_state.hpp"
#include "faio/detail/coroutine/composition/join_handle.hpp"
#include <atomic>
#include <concepts>
#include <exception>
#include <functional>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace faio {
namespace detail {
/**
 * @brief 将用户子任务包装为由调度器持有的根协程。
 * child 的所有权转入根帧；state 由正在等待排空的 scope 帧保证存活。
 */
template <class T, class S>
root_task scope_root_task_wrapper(task<T, S> child, basic_scope_state<S>* state) {
  try {
    co_await std::move(child);
  } catch (const operation_cancelled&) {
    if (!state->stop.stop_requested())
      state->fail(std::current_exception());
  } catch (...) {
    state->fail(std::current_exception());
  }
  state->done();
}
}  // namespace detail

/**
 * @brief 提供给 body 的子任务组操作视图，只借用内部状态。
 * 只有通过此对象的 spawn 启动的子任务才计入 scope 的完成计数。
 */
template <class S = default_scheduler>
class basic_scope_context {
 public:
  /** @brief scope 的静态调度器类型，用于保持 scope 的调度约束。 */
  using scheduler_type = S;

  /** @brief 借用正在执行的 scope 状态；scope 完成后不得继续使用此视图。 */
  explicit basic_scope_context(detail::basic_scope_state<S>& state) noexcept : state_(&state) {}

  /** @brief 禁止复制视图，避免在作用域外保留借用。 */
  basic_scope_context(const basic_scope_context&) = delete;

  /** @brief 禁止复制赋值，视图始终对应构造时的 scope。 */
  basic_scope_context& operator=(const basic_scope_context&) = delete;

  /**
   * @brief 子任务必须在 scope 结束前完成。失败会请求停止所有兄弟任务，
   * scope 等它们退出后才传播第一个异常，不留下引用已失效的后台任务。
   * 立即提交一个由本 scope 管理的子任务，并接管 child 的协程帧所有权。
   * 提交失败会归还本组计数，并把错误抛给 body。
   */
  template <class T>
  void spawn(task<T, S> child) {
    auto root = detail::scope_root_task_wrapper(std::move(child), state_);
    auto task_context = detail::capture_task_context();
    task_context.scope = state_->tracker;
    task_context.stop_token = state_->stop.get_token();
    task_context.cancellation_policy = state_->cancellation_policy;
    task_context.cancellation_owner = state_->cancellation_owner;
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    task_context.external_scope = state_->external_scope;
#endif
    root.set_context(std::move(task_context));
    state_->remaining.fetch_add(1, std::memory_order_relaxed);
    if (state_->tracker)
      state_->tracker->add();
    try {
      schedule_task(std::move(root), state_->scheduler, state_->tracker, state_->lifetime);
    } catch (...) {
      state_->done();
      throw;
    }
  }

  /** @brief 主动请求停止本组子任务；函数返回不代表子任务已经结束，scope 仍会等待它们全部退出。 */
  void request_stop() noexcept { state_->stop.request_stop(); }

  /** @brief 取得本组停止令牌，允许 body 的其他操作显式观察停止状态。 */
  std::stop_token stop_token() const noexcept { return state_->stop.get_token(); }

 private:
  detail::basic_scope_state<S>*
      state_; /**< 借用 scope 帧中的状态，不分配内存，也不延长其生命周期。 */
};

using scope_context = basic_scope_context<default_scheduler>;

/**
 * @brief body(scope_context&) 返回 task<T>。body 返回后仍等待其启动的所有子任务；
 * body 或任一子任务抛异常时请求停止，等待清理完成后向调用者传播异常。
 * 构造惰性的结构化作用域任务；F 按值保存在 scope 帧中。
 * 只有 co_await/block_on 启动它后才调用 body(context)，排空后返回 body 的结果。
 */
template <class S = default_scheduler, class F>
  requires std::same_as<typename std::invoke_result_t<F&, basic_scope_context<S>&>::scheduler_type,
                        S>
auto scope(F body)
    -> task<typename std::invoke_result_t<F&, basic_scope_context<S>&>::value_type, S> {
  using T = typename std::invoke_result_t<F&, basic_scope_context<S>&>::value_type;
  auto scheduler = co_await this_coro::scheduler();
  auto parent_stop = co_await this_coro::stop_token();
  auto body_target = ::faio::detail::capture_current_resume_target<S>();
  auto drain_target = ::faio::detail::capture_current_resume_target<S>();
  detail::basic_scope_state<S> state{scheduler, detail::current_task_context().scope, parent_stop};
  std::optional<std::stop_callback<detail::forward_stop>> parent_callback;
  if (!state.cancellation_owner && parent_stop.stop_possible())
    parent_callback.emplace(parent_stop, detail::forward_stop{&state.stop});
  basic_scope_context<S> context{state};
  std::optional<detail::handle_value<T>> result;
  try {
    auto body_task = std::invoke(body, context);
    auto body_state = detail::schedule_observed_task(scheduler,
                                                     std::move(body_task),
                                                     state.tracker,
                                                     state.stop.get_token(),
                                                     state.lifetime,
                                                     state.cancellation_owner);
    co_await body_state->completion.wait(std::move(body_target));
    if constexpr (std::is_void_v<T>) {
      body_state->take();
      result.emplace();
    } else {
      result.emplace(body_state->take());
    }
  } catch (...) {
    state.fail(std::current_exception());
  }
  state.done();
  co_await state.completion.wait(std::move(drain_target));
  if (state.first_error)
    std::rethrow_exception(state.first_error);
  if constexpr (std::is_void_v<T>)
    co_return;
  else
    co_return std::move(*result);
}

}  // namespace faio
