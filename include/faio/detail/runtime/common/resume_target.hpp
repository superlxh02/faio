/**
 * @file faio/detail/runtime/common/resume_target.hpp
 * @brief 默认运行时的恢复票据约定；不引入就绪队列或完整调度域。
 */
#pragma once

#include "faio/detail/coroutine/scheduler/policy.hpp"
#include "faio/detail/coroutine/scheduler/reference.hpp"
#include "faio/detail/runtime/common/constants.hpp"
#include "faio/detail/runtime/common/ready_reservation.hpp"
#include <coroutine>
#include <cstddef>

namespace faio::detail {
enum class resume_submission;
}

namespace faio::runtime::detail {
/** @brief 固定槽与本次恢复的目标 worker；具体预留和投递由完整运行时实现。 */
struct resume_reservation {
  ready_reservation<LOCAL_QUEUE_CAPACITY> slot;
  std::size_t worker{::faio::detail::no_bound_worker};

  /** @brief 预留后续恢复或提交所需容量。 */
  void reserve(scheduler_ref scheduler, ::faio::detail::task_scheduling_policy policy);
  /** @brief 提交已准备的工作。 */
  void submit(scheduler_ref scheduler,
              ::faio::detail::task_scheduling_policy policy,
              std::coroutine_handle<> handle,
              ::faio::detail::resume_submission submission) const;
};
}  // namespace faio::runtime::detail
