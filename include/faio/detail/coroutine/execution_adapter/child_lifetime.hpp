/**
 * @file faio/detail/coroutine/execution_adapter/child_lifetime.hpp
 * @brief execution 图中协程及用户等待器的子任务租约。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/coroutine/context/execution_binding.hpp"
#include "faio/detail/coroutine/execution_adapter/context.hpp"
#include "faio/detail/runtime/execution_adapter/external_work.hpp"
#include <utility>

namespace faio::detail {
/** @brief 外部执行图子任务的生命周期租约。 */
struct execution_child_lifetime {
  external_child_lease external_child;

  /** @brief 捕获外部执行子任务租约。 */
  void capture_execution_child(external_scope_ref scope) noexcept {
    if (scope.valid())
      external_child = scope.host->acquire_child(scope);
  }

  /** @brief 归还外部执行子任务租约。 */
  void finish_execution_child() noexcept { external_child.reset(); }
};

/** @brief 绑定外部执行图子任务租约。 */
template <class Root, class S>
void bind_execution_child(Root& root, basic_scheduler_ref<S> scheduler) {
  const auto scope = ::faio::detail::current_task_context().external_scope;
  if (scope.valid() && scope.host == current_external_host()
      && current_scheduler_matches(scheduler))
    root.set_external_child(scope.host->acquire_child(scope));
}

/** @brief 绑定等待器执行上下文。 */
template <bool has_parent_context, class Promise>
external_scope_ref awaiter_execution_scope(std::coroutine_handle<Promise> continuation,
                                           bool matching_thread) noexcept {
  auto external = inherited_execution_scope(matching_thread);
  if constexpr (has_parent_context)
    external = continuation.promise().context.external_scope;
  if (!matching_thread || external.host != current_external_host())
    external = {};
  return external;
}
}  // namespace faio::detail
#endif
