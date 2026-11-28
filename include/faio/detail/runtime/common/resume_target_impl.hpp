/**
 * @file faio/detail/runtime/common/resume_target_impl.hpp
 * @brief 默认运行时的恢复容量接纳、就绪投递及线程唤醒实现。
 */
#pragma once

#include "faio/detail/runtime/common/resume_target.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/domain_scheduler.hpp"
#include "faio/detail/coroutine/scheduler/resume_target.hpp"
#include <stdexcept>

namespace faio::runtime::detail {
/** @brief 预留后续恢复或提交所需容量。 */
inline void resume_reservation::reserve(scheduler_ref scheduler,
                                        ::faio::detail::task_scheduling_policy policy) {
  if (slot || (!policy.no_stealing && !policy.binds_core()))
    return;
  if (!scheduler)
    throw std::logic_error("没有可用的调度器");
  *this = scheduler.native_handle()->reserve_resume(policy);
}

/** @brief 提交已准备的工作。 */
inline void resume_reservation::submit(scheduler_ref scheduler,
                                       ::faio::detail::task_scheduling_policy policy,
                                       std::coroutine_handle<> handle,
                                       ::faio::detail::resume_submission submission) const {
  if (!scheduler)
    throw std::logic_error("没有可用的调度器");
  auto& target = *scheduler.native_handle();
  if (slot) {
    /** @brief 被调用方先取得持有所有权的副本，再发布状态，以免等待器提前恢复。 */
    target.enqueue_reserved(handle, *this);
    return;
  }
  if (policy.binds_core()) {
    target.enqueue_on(handle, policy.bound_worker);
    return;
  }
  if (policy.no_stealing) {
    target.enqueue_no_stealing(handle);
    return;
  }
  switch (submission) {
    case ::faio::detail::resume_submission::NORMAL:
      target.enqueue(handle);
      break;
    case ::faio::detail::resume_submission::IO:
      target.enqueue_io(handle);
      break;
    case ::faio::detail::resume_submission::YIELD:
      target.enqueue_yield(handle);
      break;
    case ::faio::detail::resume_submission::COOPERATIVE_YIELD:
      target.enqueue_cooperative_yield(handle);
      break;
  }
}
}  // namespace faio::runtime::detail
