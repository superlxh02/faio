#ifndef FAIO_DETAIL_RUNTIME_COMMON_BLOCKING_TASK_HPP
#define FAIO_DETAIL_RUNTIME_COMMON_BLOCKING_TASK_HPP

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/join_handle.hpp"
#include "faio/detail/coroutine/task_lifetime_ref.hpp"
#include "faio/detail/coroutine/task_tracker.hpp"
#include "faio/detail/runtime/common/blocking_pool.hpp"
#include <concepts>
#include <functional>
#include <memory>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace faio::runtime::detail {

// 阻塞任务不创建协程帧；其完成状态与派生协程任务使用相同的可等待协议。
template <class F>
  requires std::invocable<std::decay_t<F> &>
auto start_blocking(blocking_pool &pool, F &&function,
                    ::faio::detail::task_tracker *tracker,
                    std::stop_token parent_stop,
                    ::faio::task_lifetime_ref lifetime)
    -> join_handle<std::invoke_result_t<std::decay_t<F> &>> {
  using result_type = std::invoke_result_t<std::decay_t<F> &>;
  static_assert(!std::is_reference_v<result_type>, "阻塞任务不能返回引用");
  using state_type = ::faio::detail::join_handle_state<result_type>;
  auto state = std::make_shared<state_type>();
  if (parent_stop.stop_possible())
    state->parent_callback.emplace(
        parent_stop, ::faio::detail::forward_stop{&state->stop_source});

  // 先构造类型擦除后的任务，再登记运行时生命周期计数。
  ::faio::move_only_function<void()> job =
      [state, function = std::forward<F>(function), tracker,
       lifetime]() mutable {
        try {
          if (state->stop_source.stop_requested())
            throw operation_cancelled{};
          if constexpr (std::is_void_v<result_type>) {
            std::invoke(function);
            state->value.emplace();
          } else {
            state->value.emplace(std::invoke(function));
          }
        } catch (...) {
          state->error = std::current_exception();
        }
        state->publish();
        if (tracker)
          tracker->done();
        lifetime.finish_task();
      };
  lifetime.register_task();
  if (tracker)
    tracker->add();
  try {
    pool.submit(std::move(job));
  } catch (...) {
    if (tracker)
      tracker->done();
    lifetime.finish_task();
    throw;
  }
  return join_handle<result_type>{std::move(state)};
}

} // namespace faio::runtime::detail
#endif
