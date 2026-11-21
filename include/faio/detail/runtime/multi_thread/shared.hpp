/**
 * @file faio/detail/runtime/multi_thread/shared.hpp
 * @brief 多线程运行时共享组件。
 */
#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_SHARED_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_SHARED_HPP

#include "faio/detail/runtime/common/blocking_pool.hpp"
#include "faio/detail/coroutine/memory/large_frame_pool.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include "faio/detail/runtime/common/io_services.hpp"
#include "faio/detail/runtime/common/root_task_counter.hpp"
#include "faio/detail/runtime/common/completion_service.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/execution_adapter/external_work.hpp"
#endif
#include "faio/detail/runtime/multi_thread/scheduler/domain_scheduler.hpp"
#include <cstddef>
#include <latch>

namespace faio::runtime::detail {
/**
 * @brief 整个 runtime 的共享组件容器：调度域与根任务生命周期各自独立。
 * shared 先于所有 worker 构造，全部工作线程退出并销毁后才销毁。
 */
class shared {
 public:
  /** @brief 固定 worker 数同时决定调度注册表和退出屏障容量。 */
  explicit shared(const runtime_config& config)
      : config_(config),
        large_frames_(config._large_frame_pool_limit_bytes),
        io_services_(make_io_services(config)),
        io_contexts_(config._num_workers),
        scheduler_(config._num_workers),
        completion_(config._num_workers),
        blocking_(config._max_blocking_threads,
                  config._blocking_keep_alive,
                  config._blocking_queue_limit,
                  config._blocking_pool_policy),
        root_tasks_{completion_},
        workers_exited_(static_cast<std::ptrdiff_t>(config._num_workers)) {
    completion_.set_waker(basic_target_waker_ref{scheduler_});
  }

  /** @brief 禁止复制构造运行时共享组件。 */
  shared(const shared&) = delete;

  /** @brief 禁止复制赋值。 */
  shared& operator=(const shared&) = delete;

  /**  @brief 注册稳定 domain lease；worker 退出后共享域仍能安全完成关闭。 */
  void register_io(std::size_t worker, io::io_context context) noexcept {
    std::lock_guard lock(io_mutex_);
    io_services_.placement_service->register_domain(worker, context.domain());
    io_contexts_[worker] = std::move(context);
  }

  /** @brief 返回实际已初始化后端提供的能力快照。 */
  io::io_capabilities capabilities() const noexcept {
    return io_contexts_.front().domain()->capabilities();
  }

  /** @brief 取得当前线程的 I/O 服务绑定。 */
  const io::engine_config& io_services() const noexcept { return io_services_; }

  /**  @brief 停机时先通知所有分片，避免等待可能一直挂起的读取完成后才发出取消。 */
  void begin_io_shutdown(io::shutdown_policy policy) noexcept {
    std::lock_guard lock(io_mutex_);
    for (auto& context : io_contexts_)
      if (context)
        context.domain()->begin_shutdown(policy);
  }

  /**  @brief 根任务排空后，仍须等待关闭操作和结果发布者全部退出，再关闭调度域。 */
  void drain_io() noexcept {
    for (auto& context : io_contexts_)
      if (context) {
        context.domain()->wake();
        context.domain()->wait_quiescent();
      }
    close_io_services(io_services_);
  }

  /**  @brief 工作线程只借用具体页池；预算为零时显式遮蔽外层运行时的 TLS 服务。 */
  ::faio::detail::large_frame_pool* frame_pool() noexcept {
    return config_._large_frame_pool_limit_bytes ? &large_frames_ : nullptr;
  }

  /**  @brief 所有工作线程 join 后停止接收空闲映射；此后归还的映射仅借用 extent 的稳定控制对象。 */
  void retire_frame_pool() noexcept { large_frames_.retire(); }

  /** @brief 查询只读配置，不暴露调度域内部队列。 */
  const runtime_config& config() const noexcept { return config_; }

  /** @brief 借用统一调度域；所有本地调度器连接到这个成员对象。 */
  domain_scheduler& scheduler() noexcept { return scheduler_; }

  /** @brief 取得文件或通用阻塞执行服务引用，具体服务归属由上下文决定。 */
  blocking_pool& blocking() noexcept { return blocking_; }

  /** @brief 取得任务完成发布服务。 */
  auto& completion_service() noexcept { return completion_; }

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  /** @brief 取得关联的外部执行宿主。 */
  ::faio::detail::external_work_host& external_host() noexcept { return completion_; }
#endif

  /** @brief 查询纯调度引用，无生命周期计数混入调度器接口。 */
  scheduler_ref scheduler_reference() noexcept { return scheduler_ref{scheduler_}; }

  /** @brief 查询根任务生命周期借用引用，仅根帧使用，不扩大同步等待节点。 */
  task_lifetime_ref task_lifetime_reference() noexcept { return task_lifetime_ref{root_tasks_}; }

  /** @brief 外部关闭流程先等全部根帧销毁，随后才能关闭调度域。 */
  void wait_for_tasks() noexcept {
    completion_.wait_until(
        [&] { return root_tasks_.counter.count() == 0 && completion_.quiescent(); });
  }

  /** @brief 排空后关闭调度域，唤醒空闲 worker 退出事件循环。 */
  void close() { scheduler_.close(); }

  /** @brief 每个 worker 在销毁本地队列之前等待其他事件循环退出，保护无锁窃取注册表。 */
  void synchronize_worker_exit() { workers_exited_.arrive_and_wait(); }

  /**  @brief 启动失败时撤销未构造工作线程所占的退出屏障计数；已成功启动的线程仍可正常析构并 join。 */
  void drop_unstarted_workers(std::ptrdiff_t count = 1) noexcept {
    workers_exited_.count_down(count);
  }

 private:
  const runtime_config config_; /**< runtime 的不可变配置。 */
  ::faio::detail::large_frame_pool
      large_frames_; /**< 服务先于工作线程构造，在所有线程 join 后才析构并永久关闭。 */
  io::engine_config
      io_services_; /**< 先构造、最后释放，供各线程共享独立的文件、DNS 和清理执行服务。 */
  std::vector<io::io_context> io_contexts_; /**< 生命周期覆盖所有 worker 栈对象。 */
  std::mutex io_mutex_;                     /**< 保护线程唤醒设施和 IO 绑定的生命周期。 */
  domain_scheduler scheduler_;              /**< 拥有共享调度状态，不拥有 worker 或 I/O 引擎。 */
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  ::faio::detail::external_work_host completion_;
#else
  ::faio::detail::runtime_completion_service completion_;
#endif
  blocking_pool blocking_; /**< 运行时拥有的通用阻塞任务池，避免在协程工作线程执行同步阻塞调用。 */
  ::faio::detail::task_lifetime_service
      root_tasks_;            /**< 等待中的根任务也计入，独立于就绪队列长度。 */
  std::latch workers_exited_; /**< 统一退出屏障，确保没有窃取者仍访问本地队列。 */
};
}  // namespace faio::runtime::detail
#endif
