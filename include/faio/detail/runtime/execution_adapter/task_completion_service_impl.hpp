/**
 * @file faio/detail/runtime/execution_adapter/task_completion_service_impl.hpp
 * @brief 宿主完整后装配终态发布，不改变共享所有者与 publisher 协议。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/execution_adapter/task_completion_service.hpp"
#include "faio/detail/runtime/execution_adapter/external_work.hpp"

namespace faio::detail {
/** @brief 尝试发布任务终止状态。 */
inline void task_completion_service::try_terminal() noexcept {
  if (!terminal_node_ || !body_done.load(std::memory_order_acquire)
      || !tracker_zero.load(std::memory_order_acquire)
      || publishers.load(std::memory_order_acquire) != 0)
    return;
  if (terminal_queued.exchange(true, std::memory_order_acq_rel))
    return;
  terminal_keepalive = shared_from_this();
  root->host().publish(*terminal_node_);
}
}  // namespace faio::detail

#endif
