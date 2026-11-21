/**
 * @file faio/detail/process/execution_adapter/operation_scope.hpp
 * @brief 进程操作对 execution 任务图的借用、子任务与完成发布责任。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/coroutine/context/task_context.hpp"
#include "faio/detail/runtime/execution_adapter/external_work.hpp"

namespace faio::process::detail::execution_adapter {
/** @brief 为进程操作持有所属 execution 任务图的租约。 */
class operation_scope {
 public:
  /** @brief 在操作接纳前捕获所属图，未绑定 execution 图时保持空租约。 */
  void capture() noexcept {
    if (auto* host = ::faio::detail::current_external_host();
        host && ::faio::detail::current_task_context().external_scope.valid())
      root_ = host->lease(::faio::detail::current_task_context().external_scope);
  }

  /** @brief 在整个等待期间持有子任务责任，返回后外部图才可完成排空。 */
  auto acquire_child() const noexcept -> ::faio::detail::external_child_lease {
    return ::faio::detail::external_child_lease{root_};
  }

  /** @brief 在完成事件通知结束前保持租约，避免接收器过早销毁任务图。 */
  auto start_publisher() const noexcept -> ::faio::detail::external_publisher_guard {
    return ::faio::detail::external_publisher_guard{root_};
  }

 private:
  ::faio::detail::external_root_handle root_;
};
}  // namespace faio::process::detail::execution_adapter
#endif
