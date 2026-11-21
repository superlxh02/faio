/**
 * @file faio/detail/runtime/common/task_lifetime_service_impl.hpp
 * @brief 通过通用完成服务发布原生根任务归还通知。
 */
#pragma once

#include "faio/detail/runtime/common/task_lifetime_service.hpp"
#include "faio/detail/coroutine/context/execution_binding.hpp"
#include "faio/detail/runtime/common/completion_service.hpp"

namespace faio::detail {
/** @brief 归还根任务计数。 */
inline void task_lifetime_service::finish_task() noexcept {
  const bool finishing_on_owner =
      current_execution_binding() && current_task_lifetime().native_handle() == this;
  if (finishing_on_owner && counter.try_finish_nonlast())
    return;
  auto publisher = completion_
                       ? runtime_completion_service::completion_publisher_guard{*completion_}
                       : runtime_completion_service::completion_publisher_guard{};
  counter.finish_task();
  if (completion_ && !finishing_on_owner)
    completion_->notify_completion();
}
}  // namespace faio::detail
