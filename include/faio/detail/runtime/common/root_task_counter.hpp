/**
 * @file faio/detail/runtime/common/root_task_counter.hpp
 * @brief 运行时调度、线程协调与定时驱动：根任务活动计数。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#ifndef FAIO_DETAIL_RUNTIME_COMMON_ROOT_TASK_COUNTER_HPP
#define FAIO_DETAIL_RUNTIME_COMMON_ROOT_TASK_COUNTER_HPP

#include <atomic>
#include <cassert>
#include <cstddef>

namespace faio::runtime::detail {
/// @brief 整个 runtime 的根任务计数，与 block_on 的局部 task_tracker 分开。
/// 等待中的根任务即使不在就绪队列里，也必须阻止运行时提前关闭。
class root_task_counter {
 public:
  /// @brief 投递前增加计数，确保在其他线程立即完成的任务也被计入生命周期。
  void register_task() noexcept { active_tasks_.fetch_add(1, std::memory_order_relaxed); }

  /// @brief 根帧析构时归还一次；最后一个根任务唤醒等待排空的外部线程。
  void finish_task() noexcept {
    const auto previous = active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    assert(previous != 0);
    if (previous == 1)
      active_tasks_.notify_all();
  }

  /// @brief 外部线程等待全部根帧退出；调用方先停止接受新的外部根任务。
  void wait() const noexcept {
    for (auto count = active_tasks_.load(std::memory_order_acquire); count != 0;
         count = active_tasks_.load(std::memory_order_acquire))
      active_tasks_.wait(count, std::memory_order_acquire);
  }

  std::size_t count() const noexcept { return active_tasks_.load(std::memory_order_acquire); }

 private:
  std::atomic<std::size_t> active_tasks_{0};  ///< 已登记且尚未完成析构的根任务数。
};
}
#endif
