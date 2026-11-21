/**
 * @file faio/detail/coroutine/composition/shared_task.hpp
 * @brief 为词法任务组持有的共享工作提供不可单独取消的观察接口。
 */
#pragma once

#include "faio/detail/coroutine/composition/shared_task_state.hpp"

namespace faio {
template <class T, class S>
class shared_task;

/** @brief 持有共享终态，在对象的生命周期内提供只读结果。 */
template <class T, class S = default_scheduler>
class shared_result {
 public:
  /** @brief 取得稳定结果的引用，不复制结果；失败时可重复抛出异常。 */
  const detail::handle_value<T>& value() const& {
    if (state_->failure)
      std::rethrow_exception(state_->failure);
    return *state_->value;
  }

  const detail::handle_value<T>& value() const&& = delete;

  /** @brief 查询已发布异常。 */
  std::exception_ptr exception() const noexcept { return state_->failure; }

 private:
  friend class shared_task<T, S>;

  explicit shared_result(std::shared_ptr<detail::shared_task_state<T, S>> state) noexcept
      : state_(std::move(state)) {}

  std::shared_ptr<detail::shared_task_state<T, S>> state_;
};

/** @brief 可复制的共享观察句柄。等待不可单独取消，生产者仍受原任务组的停止策略约束。 */
template <class T, class S = default_scheduler>
class [[nodiscard]] shared_task {
 public:
  /**
   * @brief 按原生事件协议等待，取得持有结果的对象。
   * @details 支持同一静态调度器类型的调用者，并使用其自己的恢复目标。
   */
  task<shared_result<T, S>, S> observe_uncancellable() const { return observe(state_); }

 private:
  friend class basic_task_group_context<S>;
  using state_type = detail::shared_task_state<T, S>;

  explicit shared_task(std::shared_ptr<state_type> state) noexcept : state_(std::move(state)) {}

  static task<shared_result<T, S>, S> observe(std::shared_ptr<state_type> state) {
    if (!state)
      throw std::logic_error("共享任务观察句柄为空");
    co_await state->ready.wait();
    co_return shared_result<T, S>{std::move(state)};
  }

  std::shared_ptr<state_type> state_;
};

template <class S>
template <class T>
auto basic_task_group_context<S>::spawn_shared_uncancellable(task<T, S> child) {
  using state_type = detail::shared_task_state<T, S>;
  auto state = std::make_shared<state_type>();
  (void)spawn_observed(state_type::produce(std::move(child), state));
  return shared_task<T, S>{std::move(state)};
}
}  // namespace faio
