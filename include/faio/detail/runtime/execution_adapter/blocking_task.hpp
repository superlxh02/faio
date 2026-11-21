/**
 * @file faio/detail/runtime/execution_adapter/blocking_task.hpp
 * @brief 阻塞任务对 execution 外部任务图的作用域和子任务责任。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/runtime/execution_adapter/external_work.hpp"
#include "faio/detail/coroutine/context/task_context.hpp"

namespace faio::runtime::detail {
/** @brief 执行图关联的阻塞任务上下文。 */
struct blocking_task_execution {
  /** @brief 构造执行图关联的阻塞任务上下文。 */
  explicit blocking_task_execution(::faio::detail::external_work_host* owner) noexcept
      : host(owner) {}

  /** @brief 继承父任务作用域。 */
  void inherit_scope(bool same_host) noexcept {
    external_scope = same_host && ::faio::detail::current_task_context().external_scope.valid()
                             && ::faio::detail::current_task_context().external_scope.host == host
                         ? ::faio::detail::current_task_context().external_scope
                         : ::faio::detail::external_scope_ref{};
    external_child = external_scope.valid() ? host->acquire_child(external_scope)
                                            : ::faio::detail::external_child_lease{};
  }

  ::faio::detail::external_work_host* host;
  ::faio::detail::external_scope_ref external_scope;
  ::faio::detail::external_child_lease external_child;
};
}  // namespace faio::runtime::detail
#endif
