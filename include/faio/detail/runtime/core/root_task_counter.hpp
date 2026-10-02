#ifndef FAIO_DETAIL_RUNTIME_CORE_ROOT_TASK_COUNTER_HPP
#define FAIO_DETAIL_RUNTIME_CORE_ROOT_TASK_COUNTER_HPP

#include <atomic>
#include <cassert>
#include <cstddef>

namespace faio::runtime::detail {
// 整个 runtime 的根任务计数，与 block_on 的局部 task_tracker 分开。
// 等待中的根任务即使不在就绪队列里，也必须阻止运行时提前关闭。
class root_task_counter {
public:
  // 投递前增加计数，确保在其他线程立即完成的任务也被计入生命周期。
  void register_task() noexcept { active_tasks_.fetch_add(1, std::memory_order_relaxed); }
  // 根帧析构时归还一次；最后一个根任务唤醒等待排空的外部线程。
  void finish_task() noexcept {
    const auto previous = active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    assert(previous != 0);
    if (previous == 1) active_tasks_.notify_all();
  }
  // 外部线程等待全部根帧退出；调用方先停止接受新的外部根任务。
  void wait() const noexcept {
    for (auto count = active_tasks_.load(std::memory_order_acquire); count != 0;
         count = active_tasks_.load(std::memory_order_acquire))
      active_tasks_.wait(count, std::memory_order_acquire);
  }
private:
  std::atomic<std::size_t> active_tasks_{0}; // 已登记且尚未完成析构的根任务数。
};
} // namespace faio::runtime::detail
#endif // FAIO_DETAIL_RUNTIME_CORE_ROOT_TASK_COUNTER_HPP
