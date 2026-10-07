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
  requires std::invocable<std::decay_t<F>&>
auto start_blocking(blocking_pool& pool,
                    F&& function,
                    ::faio::detail::task_tracker* tracker,
                    std::stop_token parent_stop,
                    ::faio::task_lifetime_ref lifetime,
                    ::faio::scheduler_ref target = ::faio::detail::current_scheduler(),
                    ::faio::detail::external_work_host* host = ::faio::detail::current_external_host())
    -> join_handle<std::invoke_result_t<std::decay_t<F>&>> {
  using result_type = std::invoke_result_t<std::decay_t<F>&>;
  static_assert(!std::is_reference_v<result_type>, "阻塞任务不能返回引用");
  using state_type = ::faio::detail::join_handle_state<result_type>;
  auto state = std::make_shared<state_type>();
  const bool same_host = target && host == ::faio::detail::current_external_host()
                         && target == ::faio::detail::current_scheduler();
  state->cancellation_policy = same_host ? ::faio::detail::current_cancellation_policy
      : ::faio::detail::cancellation_error_policy::fatal_if_unobserved;
  const auto external_scope = same_host && ::faio::detail::current_external_scope.valid()
      && ::faio::detail::current_external_scope.host == host
      ? ::faio::detail::current_external_scope : ::faio::detail::external_scope_ref{};
  if (state->cancellation_policy == ::faio::detail::cancellation_error_policy::normal_if_stop_requested
      || (same_host && ::faio::detail::current_cancellation_owner)) {
    state->cancellation_owner = std::make_shared<::faio::detail::cancellation_state>(parent_stop,
        same_host ? ::faio::detail::current_cancellation_owner : nullptr);
    state->stop_source = state->cancellation_owner->source;
  }
  if (!state->cancellation_owner && parent_stop.stop_possible())
    state->parent_callback.emplace(parent_stop, ::faio::detail::forward_stop{&state->stop_source});
  auto external_child = external_scope.valid() ? host->acquire_child(external_scope)
      : ::faio::detail::external_child_lease{};

  // 先构造类型擦除后的任务，再登记运行时生命周期计数。
  ::faio::move_only_function<void()> job =
      [state, function = std::forward<F>(function), tracker, lifetime, target, host,
       pool = &pool, external_scope, external_child = std::move(external_child)]() mutable {
        ::faio::detail::scoped_submission_binding submission{target, lifetime, *pool, host};
        ::faio::detail::scoped_task_context context{{.scope = tracker,
            .scheduler = target, .stop_token = state->stop_source.get_token(),
            .cancellation_policy = state->cancellation_policy, .external_scope = external_scope,
            .cancellation_owner = state->cancellation_owner}};
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
          state->capture_error(std::current_exception());
        }
        state->publish();
        auto completion = tracker ? tracker->done() : ::faio::detail::task_tracker::completion_guard{};
        lifetime.finish_task();
        external_child.reset();
      };
  lifetime.register_task();
  if (tracker)
    tracker->add();
  try {
    pool.submit(std::move(job));
  } catch (...) {
    auto completion = tracker ? tracker->done() : ::faio::detail::task_tracker::completion_guard{};
    lifetime.finish_task();
    throw;
  }
  return join_handle<result_type>{std::move(state)};
}
}  // namespace faio::runtime::detail
#endif
