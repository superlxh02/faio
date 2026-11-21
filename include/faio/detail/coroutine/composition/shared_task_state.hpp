/**
 * @file faio/detail/coroutine/composition/shared_task_state.hpp
 * @brief 组内共享任务独立存储的终态与广播事件。
 */
#pragma once

#include "faio/detail/coroutine/composition/task_group.hpp"

namespace faio::detail {
/** @brief 单发布、多读取的共享终态。 */
template <class T, class S>
struct shared_task_state {
  std::optional<handle_value<T>> value;
  std::exception_ptr failure;
  basic_completion_event<S> ready;

  /** @brief 完成后允许所有观察者并行取得稳定只读结果。 */
  static task<void, S> produce(task<T, S> work, std::shared_ptr<shared_task_state> state) {
    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(work);
        state->value.emplace();
      } else {
        state->value.emplace(co_await std::move(work));
      }
    } catch (...) {
      state->failure = std::current_exception();
      state->ready.notify();
      throw;
    }
    state->ready.notify();
  }
};
}  // namespace faio::detail
