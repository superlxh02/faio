/**
 * @file faio/detail/coroutine/lifetime/tracker.hpp
 * @brief 任务组完成计数。
 */
#ifndef FAIO_DETAIL_COROUTINE_TASK_TRACKER_HPP
#define FAIO_DETAIL_COROUTINE_TASK_TRACKER_HPP

#include "faio/detail/coroutine/lifetime/tracker_fwd.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/execution_adapter/tracker_completion.hpp"
#endif
#include <atomic>
#include <cstddef>

namespace faio::detail {
/**
 * @brief 统计执行作用域内尚未结束的根任务，仅在根任务启动和完成时修改。
 * 通常由调用 block_on 的普通线程在栈上持有。
 * 登记者必须在外层计数归零前完成所有派生任务登记，根帧结束再归还。
 */
struct task_tracker
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    : task_tracker_completion
#endif
{
  std::atomic<std::size_t> pending{0};

  void add() noexcept { pending.fetch_add(1, std::memory_order_relaxed); }

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  completion_guard done() noexcept { return complete(pending); }
#else
  void done() noexcept {
    if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
      pending.notify_all();
  }
#endif

  void wait() const noexcept {
    for (auto n = pending.load(std::memory_order_acquire); n != 0;
         n = pending.load(std::memory_order_acquire))
      pending.wait(n, std::memory_order_acquire);
  }
};

}  // namespace faio::detail
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/common/coroutine_integration.hpp"
#endif
#endif
