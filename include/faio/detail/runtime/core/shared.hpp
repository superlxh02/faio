#ifndef FAIO_DETAIL_RUNTIME_CORE_SHARED_HPP
#define FAIO_DETAIL_RUNTIME_CORE_SHARED_HPP

#include "faio/detail/runtime/core/config.hpp"
#include "faio/detail/runtime/core/root_task_counter.hpp"
#include "faio/detail/runtime/core/scheduler/domain_scheduler.hpp"
#include <cstddef>
#include <latch>

namespace faio::runtime::detail {
// 整个 runtime 的共享组件容器：调度域与根任务生命周期各自独立。
// shared 先于所有 worker 构造，全部工作线程退出并销毁后才销毁。
class shared {
public:
  // 固定 worker 数同时决定调度注册表和退出屏障容量。
  explicit shared(const runtime_config& config)
      : config_(config), scheduler_(config._num_workers),
        workers_exited_(static_cast<std::ptrdiff_t>(config._num_workers)) {}
  shared(const shared&) = delete;
  shared& operator=(const shared&) = delete;
  // 查询只读配置，不暴露调度域内部队列。
  const runtime_config& config() const noexcept { return config_; }
  // 借用统一调度域；所有本地调度器连接到这个成员对象。
  domain_scheduler& scheduler() noexcept { return scheduler_; }
  // 查询纯调度引用，无生命周期计数混入调度器接口。
  scheduler_ref scheduler_reference() noexcept { return scheduler_ref{scheduler_}; }
  // 查询根任务生命周期借用引用，仅根帧使用，不扩大同步等待节点。
  task_lifetime_ref task_lifetime_reference() noexcept { return task_lifetime_ref{root_tasks_}; }
  // 外部关闭流程先等全部根帧销毁，随后才能关闭调度域。
  void wait_for_tasks() const noexcept { root_tasks_.wait(); }
  // 排空后关闭调度域，唤醒空闲 worker 退出事件循环。
  void close() { scheduler_.close(); }
  // 每个 worker 在销毁本地队列之前等待其他事件循环退出，保护无锁窃取注册表。
  void synchronize_worker_exit() { workers_exited_.arrive_and_wait(); }
private:
  const runtime_config config_;             // runtime 的不可变配置。
  domain_scheduler scheduler_;      // 拥有共享调度状态，不拥有 worker 或 I/O 引擎。
  root_task_counter root_tasks_;    // 等待中的根任务也计入，独立于就绪队列长度。
  std::latch workers_exited_;       // 统一退出屏障，确保没有窃取者仍访问本地队列。
};
} // namespace faio::runtime::detail
#endif // FAIO_DETAIL_RUNTIME_CORE_SHARED_HPP
