/**
 * @file faio/detail/coroutine/composition/scope_state.hpp
 * @brief 结构化并发作用域的帧内共享状态。
 */
#pragma once

#include "faio/detail/coroutine/context/execution_binding.hpp"
#include "faio/detail/coroutine/context/current_context.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/execution_adapter/context.hpp"
#endif
#include "faio/detail/coroutine/lifetime/tracker.hpp"
#include "faio/detail/coroutine/wait/completion_event.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

namespace faio::detail {
/**
 * @brief 结构化任务组的内部状态，存放于 scope 的协程帧。
 * 所有子任务只借用该状态；scope 排空子任务后才允许销毁这块存储。
 */
template <class S>
struct basic_scope_state
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    : execution_scope_context
#endif
{
  basic_scope_state(scheduler_ref_for<S> target, task_tracker* outer, std::stop_token parent_stop)
      : scheduler(target), tracker(outer) {
    if (cancellation_policy == cancellation_error_policy::normal_if_stop_requested
        || current_task_context().cancellation_owner) {
      cancellation_owner = std::make_shared<cancellation_state>(
          parent_stop, current_task_context().cancellation_owner);
      stop = cancellation_owner->source;
    }
  }

  scheduler_ref_for<S> scheduler;
  ::faio::detail::task_tracker* tracker;
  task_lifetime_ref lifetime{current_task_lifetime()};
  cancellation_error_policy cancellation_policy{current_task_context().cancellation_policy};
  std::shared_ptr<cancellation_state> cancellation_owner;
  std::stop_source stop;
  std::atomic<std::size_t> remaining{1};
  ::faio::detail::basic_completion_event<S> completion;
  std::mutex error_mutex;
  std::exception_ptr first_error;

  void fail(std::exception_ptr error) {
    {
      std::lock_guard lock(error_mutex);
      if (!first_error)
        first_error = std::move(error);
    }
    stop.request_stop();
  }

  void done() {
    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
      completion.notify();
  }
};

using scope_state = basic_scope_state<default_scheduler>;

}  // namespace faio::detail
