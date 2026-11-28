/**
 * @file faio/detail/coroutine/composition/task_group.hpp
 * @brief 词法任务组、子结果领取与组退出。
 */
#pragma once

#include "faio/detail/coroutine/composition/settle.hpp"
#include "faio/detail/coroutine/composition/task_group_state.hpp"
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace faio {
template <class S>
class basic_task_group_context;

namespace detail {
struct group_lifetime_access;
/** @brief 固定窗口在投递一批子任务之前预留唯一的结果恢复目标。 */
template <class T, class S>
task<std::vector<task_outcome<T>>, S> join_limited_body(basic_task_group_context<S>&,
                                                        std::size_t,
                                                        std::vector<task<T, S>>);
}  // namespace detail

/** @brief 仅允许主体在其词法区域内一次性领取已接纳任务的结果。 */
template <class T, class S = default_scheduler>
class [[nodiscard]] scoped_join_handle {
 public:
  using scheduler_type = S;

  scoped_join_handle(scoped_join_handle&&) noexcept = default;
  scoped_join_handle& operator=(scoped_join_handle&&) noexcept = default;
  scoped_join_handle(const scoped_join_handle&) = delete;
  scoped_join_handle& operator=(const scoped_join_handle&) = delete;

  /** @brief 持有结果槽并按原生事件协议等待。 */
  struct awaiter {
    using scheduler_type = S;

    std::shared_ptr<detail::group_value_slot<T, S>> slot;
    typename detail::basic_completion_event<S>::awaiter ready;

    bool await_ready() const noexcept { return ready.await_ready(); }

    template <class Promise>
    bool await_suspend(std::coroutine_handle<Promise> parent) {
      return ready.await_suspend(parent);
    }

    task_outcome<T> await_resume() const {
      if (!slot->body_active->load(std::memory_order_acquire))
        throw std::logic_error("组结果句柄已离开主体作用域");
      return std::move(*slot->outcome);
    }
  };

  /** @brief 转交唯一消费权，暂未完成时等待子任务释放工作责任。 */
  awaiter operator co_await() & { return wait({}); }

  /** @brief 右值等待也转交唯一消费权。 */
  awaiter operator co_await() && { return this->operator co_await(); }

 private:
  template <class>
  friend class basic_task_group_context;
  friend task<std::vector<task_outcome<T>>, S> detail::join_limited_body<T, S>(
      basic_task_group_context<S>&, std::size_t, std::vector<task<T, S>>);

  /** @brief 领取唯一结果，支持固定窗口传入已预留的恢复目标。 */
  awaiter wait(detail::basic_resume_target<S> prepared) {
    if (!slot_ || !slot_->body_active->load(std::memory_order_acquire))
      throw std::logic_error("组结果句柄为空或已离开主体作用域");
    auto slot = std::move(slot_);
    auto ready = slot->ready->wait(std::move(prepared));
    return {std::move(slot), std::move(ready)};
  }

  explicit scoped_join_handle(std::shared_ptr<detail::group_value_slot<T, S>> slot) noexcept
      : slot_(std::move(slot)) {}

  std::shared_ptr<detail::group_value_slot<T, S>> slot_;
};

namespace detail {
/**
 * @brief 在回收协程帧前保存输出，归还任务组计数后才唤醒结果领取者。
 * @details 归还任务组计数后，只有独立的通知事件继续存活，根协程包装器不再持有组或业务结果。
 */
template <class T, class S>
root_task task_group_root_task_wrapper(task<T, S> work,
                                       basic_task_group_state<S>* state,
                                       std::shared_ptr<group_child_result> metadata,
                                       std::weak_ptr<group_value_slot<T, S>> observer,
                                       std::shared_ptr<basic_completion_event<S>> ready) {
  {
    std::optional<task_outcome<T>> captured;
    try {
      captured.emplace(co_await settling_awaiter<T, S>{std::move(work).operator co_await()});
    } catch (...) {
      captured.emplace(task_outcome<T>::from_error(std::current_exception()));
    }
    metadata->status = captured->status();
    metadata->exception = captured->exception();
    if (auto slot = observer.lock())
      slot->outcome.emplace(std::move(*captured));
  }

  if (metadata->status == task_outcome_status::ERROR_VALUE)
    state->observe_error();
  metadata.reset();
  state->finish();
  ready->notify();
}

/** @brief 运行主体时沿用主体的静态调度，显式安装新组的停止源。 */
template <class T, class S>
struct task_group_body_awaiter {
  using scheduler_type = S;

  settling_awaiter<T, S> work;
  const std::shared_ptr<linked_stop_state>& stop_owner;

  bool await_ready() const noexcept { return work.await_ready(); }

  template <class Promise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> parent) {
    return work.child.await_suspend_impl(parent, stop_owner);
  }

  task_outcome<T> await_resume() { return work.await_resume(); }
};
}  // namespace detail

/** @brief 提供组主体词法范围内的任务接纳权限，引用仅在主体完成前有效。 */
template <class S = default_scheduler>
class basic_task_group_context {
 public:
  using scheduler_type = S;

  explicit basic_task_group_context(detail::basic_task_group_state<S>& state) noexcept
      : state_(state) {}

  basic_task_group_context(const basic_task_group_context&) = delete;
  basic_task_group_context& operator=(const basic_task_group_context&) = delete;

  /** @brief 接纳并立即投递一个子任务，提交失败抛出并完整回滚接纳。 */
  template <class T>
  scoped_join_handle<T, S> spawn_observed(task<T, S> child) {
    return std::move(*spawn(child, false));
  }

  /** @brief 提前接纳共享工作，订阅等待不可单独取消。 */
  template <class T>
  auto spawn_shared_uncancellable(task<T, S> child);

  /** @brief 关闭后拒绝新工作，现有句柄在主体退出前仍可领取。 */
  void close() { state_.close(); }

  /** @brief 向组内工作请求协作停止。 */
  void request_stop() noexcept { state_.request_stop(); }

  /** @brief 取得此组独立的停止令牌。 */
  std::stop_token stop_token() const noexcept { return state_.stop_owner->source.get_token(); }

 private:
  friend struct detail::group_lifetime_access;

  template <class T, class Scheduler>
  friend task<std::vector<task_outcome<T>>, Scheduler> detail::join_limited_body(
      basic_task_group_context<Scheduler>&, std::size_t, std::vector<task<T, Scheduler>>);

  /** @brief 仅在组尚未收到停止请求时接纳，停止拒绝时保持输入任务所有权。 */
  template <class T>
  std::optional<scoped_join_handle<T, S>> try_spawn_observed(task<T, S>& child) {
    return spawn(child, true);
  }

  template <class T>
  std::optional<scoped_join_handle<T, S>> spawn(task<T, S>& child, bool stop_sensitive) {
    if (!detail::current_scheduler_matches(state_.scheduler))
      throw std::logic_error("启动组子任务需要组所在的调度器");
    auto metadata = state_.admit(stop_sensitive);
    if (!metadata)
      return std::nullopt;
    try {
      auto result = std::make_shared<detail::group_value_slot<T, S>>(state_.body_active);
      auto root = detail::task_group_root_task_wrapper<T, S>(
          std::move(child), &state_, metadata, std::weak_ptr{result}, result->ready);
      auto context = state_.parent;
      context.stop_token = stop_token();
      context.cancellation_owner = state_.stop_owner;
      root.set_context(std::move(context));
      if (state_.parent.scope)
        state_.parent.scope->add();
      detail::schedule_task(
          std::move(root), state_.scheduler, state_.parent.scope, state_.lifetime);
      return scoped_join_handle<T, S>{std::move(result)};
    } catch (...) {
      state_.rollback(metadata);
      throw;
    }
  }

  detail::basic_task_group_state<S>& state_;
};

namespace detail {
/** @brief 为持有自身结果状态的组合登记任务组计数，该计数只跟踪生命周期。 */
struct group_lifetime_access {
  template <class S>
  static basic_task_group_state<S>& state(basic_task_group_context<S>& group) noexcept {
    return group.state_;
  }

  template <class S>
  static void schedule_task(basic_task_group_context<S>& group,
                            root_task root,
                            const std::shared_ptr<linked_stop_state>& stop) {
    auto& owner = group.state_;
    if (!current_scheduler_matches(owner.scheduler))
      throw std::logic_error("启动组内竞速需要组所在的调度器");
    auto context = owner.parent;
    context.stop_token = stop->source.get_token();
    context.cancellation_owner = stop;
    root.set_context(std::move(context));
    owner.admit_lifetime();
    if (owner.parent.scope)
      owner.parent.scope->add();
    try {
      ::faio::detail::schedule_task(
          std::move(root), owner.scheduler, owner.parent.scope, owner.lifetime);
    } catch (...) {
      owner.finish();
      throw;
    }
  }
};
}  // namespace detail

using task_group_context = basic_task_group_context<>;

/**
 * @brief 惰性地启动主体，关闭接纳后排空所有已接纳任务，再交付结果。
 * @details 只有主体的词法范围可使用组视图与消费句柄。丢弃句柄不会停止工作或掩盖异常。
 * 主体应在组所在调度器内使用视图，并负责在主体结束前收回任何共享的视图引用。
 * 组内子任务不得借用在主体返回前已析构的局部值；组返回前持续有效的数据可在子任务中借用。
 * 若调用者把句柄存到主体之外，主体结束后拒绝领取；仍保存的句柄可延长其结果槽的寿命。
 */
template <class S = default_scheduler,
          class body_type,
          class work_type = std::invoke_result_t<body_type&, basic_task_group_context<S>&>,
          class T = typename work_type::value_type>
  requires std::same_as<work_type, task<T, S>>
task<group_result<T>, S> task_group(group_options options, body_type body) {
  auto scheduler = co_await this_coro::scheduler();
  auto drain_target = detail::capture_current_resume_target<S>();
  detail::basic_task_group_state<S> state{scheduler, detail::capture_task_context(), options};
  basic_task_group_context<S> context{state};
  std::optional<task_outcome<T>> result;
  try {
    auto work = std::invoke(body, context);
    result.emplace(co_await detail::task_group_body_awaiter<T, S>{
        {std::move(work).operator co_await()}, state.stop_owner});
  } catch (...) {
    result.emplace(task_outcome<T>::from_error(std::current_exception()));
  }
  state.close_body();
  if (result->status() == task_outcome_status::ERROR_VALUE)
    state.observe_error();
  state.finish();
  co_await state.completion.wait(std::move(drain_target));
  co_return group_result<T>{std::move(*result), state.collect()};
}

/** @brief 使用默认 fail-fast 策略运行组。 */
template <class S = default_scheduler, class body_type>
auto task_group(body_type body) {
  return task_group<S>(group_options{}, std::move(body));
}
}  // namespace faio
