/**
 * @file faio/detail/coroutine/execution_adapter/tracker_completion.hpp
 * @brief execution 桥接的任务组终态通知；发布者守卫覆盖根任务清理尾段。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/execution_adapter/completion_reference.hpp"
#include <atomic>
#include <cstddef>
#include <utility>

namespace faio::detail {
/** @brief 仅启用 execution 时为 task_tracker 附加完成发布状态。 */
class task_tracker_completion {
 public:
  /** @brief 不借用 tracker；归零通知可能使 tracker 的生命周期结束，发布服务仍须在后续清理期间保持有效。 */
  class completion_guard {
   public:
    /** @brief 构造完成发布保护。 */
    completion_guard() noexcept = default;

    /** @brief 构造完成发布保护。 */
    explicit completion_guard(task_completion_ref completion) noexcept : completion_(completion) {}

    /** @brief 禁止复制构造完成发布保护。 */
    completion_guard(const completion_guard&) = delete;
    /** @brief 禁止复制赋值。 */
    completion_guard& operator=(const completion_guard&) = delete;

    /** @brief 移动构造完成发布保护。 */
    completion_guard(completion_guard&& other) noexcept
        : completion_(std::exchange(other.completion_, {})) {}

    /** @brief 销毁完成发布保护。 */
    ~completion_guard() { completion_.end_publisher(); }

   private:
    task_completion_ref completion_;
  };

  /** @brief 添加根任务之前绑定具体发布服务，不接受借用回调或操作表。 */
  void bind_completion(task_completion_ref completion) noexcept { completion_ = completion; }

 protected:
  /** @brief 调用方持有返回的守卫，直到运行时计数和外部子任务租约全部归还。 */
  completion_guard complete(std::atomic<std::size_t>& pending) noexcept {
    const auto completion = completion_;
    completion.begin_publisher();
    completion_guard publisher{completion};
    if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      const bool call_zero = completion && !zero_fired_.exchange(true, std::memory_order_acq_rel);
      pending.notify_all();
      if (call_zero)
        completion.mark_tracker_zero();
    }
    return publisher;
  }

 private:
  task_completion_ref completion_;
  std::atomic<bool> zero_fired_{false};
};
}  // namespace faio::detail
#endif
