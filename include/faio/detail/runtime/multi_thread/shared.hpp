#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_SHARED_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_SHARED_HPP

#include "faio/detail/runtime/common/blocking_pool.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include "faio/detail/runtime/common/io_services.hpp"
#include "faio/detail/runtime/common/root_task_counter.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/domain_scheduler.hpp"
#include <cstddef>
#include <latch>

namespace faio::runtime::detail {
// 整个 runtime 的共享组件容器：调度域与根任务生命周期各自独立。
// shared 先于所有 worker 构造，全部工作线程退出并销毁后才销毁。
class shared {
 public:
  // 固定 worker 数同时决定调度注册表和退出屏障容量。
  explicit shared(const runtime_config& config)
      : config_(config),
        io_services_(make_io_services(config)),
        io_contexts_(config._num_workers),
        scheduler_(config._num_workers),
        blocking_(config._max_blocking_threads,
                  config._blocking_keep_alive,
                  config._blocking_queue_limit),
        workers_exited_(static_cast<std::ptrdiff_t>(config._num_workers)) {}

  shared(const shared&) = delete;

  shared& operator=(const shared&) = delete;

  /** @brief 注册稳定 domain lease；worker 退出后共享域仍能安全完成关闭。 */
  void register_io(std::size_t worker, io::io_context context) noexcept {
    std::lock_guard lock(io_mutex_);
    io_services_.placement_service->register_domain(worker, context.domain());
    io_contexts_[worker] = std::move(context);
  }

  io::io_capabilities capabilities() const noexcept {
    return io_contexts_.front().domain()->capabilities();
  }

  const io::engine_config& io_services() const noexcept { return io_services_; }

  /** @brief 停机先通知全部 shard，不能等待永远 pending 的 read 后才取消。 */
  void begin_io_shutdown(io::shutdown_policy policy) noexcept {
    std::lock_guard lock(io_mutex_);
    for (auto& context : io_contexts_)
      if (context)
        context.domain()->begin_shutdown(policy);
  }

  /** @brief 根任务排空后仍须排空 close/publisher，再关闭调度域。 */
  void drain_io() noexcept {
    // worker 仍可能驱动；quiescent 为稳定判断，不需要第二个 poller 抢 session。
    for (auto& context : io_contexts_)
      if (context) {
        context.domain()->wake();
        context.domain()->wait_quiescent();
      }
    close_io_services(io_services_);
  }

  // 查询只读配置，不暴露调度域内部队列。
  const runtime_config& config() const noexcept { return config_; }

  // 借用统一调度域；所有本地调度器连接到这个成员对象。
  domain_scheduler& scheduler() noexcept { return scheduler_; }

  blocking_pool& blocking() noexcept { return blocking_; }

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
  const runtime_config config_;    // runtime 的不可变配置。
  io::engine_config io_services_;  ///< 先构造、最后释放，共享专属文件/DNS/cleanup执行服务。
  std::vector<io::io_context> io_contexts_;  ///< 生命周期覆盖所有 worker 栈对象。
  std::mutex io_mutex_;
  domain_scheduler scheduler_;  // 拥有共享调度状态，不拥有 worker 或 I/O 引擎。
  blocking_pool blocking_;
  root_task_counter root_tasks_;  // 等待中的根任务也计入，独立于就绪队列长度。
  std::latch workers_exited_;     // 统一退出屏障，确保没有窃取者仍访问本地队列。
};
}  // namespace faio::runtime::detail
#endif
