/**
 * @file faio/detail/runtime/multi_thread/worker.hpp
 * @brief 工作线程事件循环。
 */
#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_WORKER_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_WORKER_HPP

#include "faio/detail/coroutine/context/context_scope.hpp"
#include "faio/detail/common/util/cpu_relax.hpp"
#include "faio/detail/coroutine/memory/frame_allocator.hpp"
#include "faio/detail/coroutine/memory/large_frame_pool_binding.hpp"
#include "faio/detail/coroutine/context/task_context.hpp"
#include "faio/detail/runtime/common/io_engine.hpp"
#include "faio/detail/runtime/common/worker_io_budget.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/local_scheduler.hpp"
#include "faio/detail/runtime/multi_thread/shared.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/execution_adapter/driver.hpp"
#endif
#include "faio/detail/log/logger.hpp"
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace faio::runtime::detail {
/** @brief 工作线程事件循环，组合本地调度与 I/O；调度队列和窃取算法由调度器管理。 */
class worker {
 public:
  /** @brief I/O 引擎先构造，再登记本地调度器，保证被注册的唤醒端始终有效。 */
  worker(shared& shared_state, std::size_t worker_id)
      : shared_(shared_state),
        worker_id_(worker_id),
        io_engine_(shared_state.config(), true, shared_state.io_services()),
        io_waker_(*io_engine_.context().domain()),
        scheduler_(shared_state.scheduler(),
                   worker_id,
                   shared_state.config()._global_queue_interval,
                   basic_worker_waker_ref{io_waker_},
                   shared_state.config()._no_stealing_queue_capacity),
        binding_(shared_state.scheduler_reference(),
                 shared_state.task_lifetime_reference(),
                 scheduler_.local_state(),
                 worker_id,
                 &shared_state.blocking()
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
                     ,
                 &shared_state.external_host()
#endif
                     ,
                 shared_state.config()._worker_cpus.empty()
                     ? -1
                     : shared_state.config()._worker_cpus[worker_id]),
        thread_guard_(binding_) {
    shared_.register_io(worker_id_, io_engine_.context());
  }

  /** @brief 禁止复制构造工作线程。 */
  worker(const worker&) = delete;

  /** @brief 禁止复制赋值。 */
  worker& operator=(const worker&) = delete;

  /** @brief 先确保所有事件循环都停止，再按成员逆序撤销绑定、注销队列并销毁 I/O。 */
  ~worker() { shared_.synchronize_worker_exit(); }

  /**
   * @brief 驱动就绪协程与 I/O；调度算法由 local_scheduler 负责。
   * @details 每轮按周期驱动 IO、处理外部节点或领取就绪协程；本域没有进展后
   * 才尝试窃取。持续就绪期间在恢复边界检查 IO 延迟预算；真正入睡前先登记
   * 休眠再复查所有就绪来源，防止工作发布与内核等待之间丢失唤醒。
   */
  void run() {
    ::faio::detail::large_frame_pool_binding large_frames{shared_.frame_pool()};
    ::faio::detail::coroutine_frame_cache_scope frame_cache;
    faio::log::logger()->debug("worker {} started", worker_id_);
    while (!shutdown_) {
      ++tick_;
      global_check_pending_ =
          global_check_pending_ || tick_ % shared_.config()._global_queue_interval == 0;
      if (tick_ % shared_.config()._io_interval == 0) {
        drive_io();
        shutdown_ = shared_.scheduler().closed();
        if (shutdown_)
          break;
      }
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
      if (execution_.preferred(shared_.external_host(), worker_id_)) {
        if (refresh_io_before_execute())
          break;
        execution_.execute(shared_.external_host(), scheduler_, worker_id_);
        idle_turns_ = 0;
        continue;
      }
#endif
      const auto poll_global = std::exchange(global_check_pending_, false);
      if (auto task = scheduler_.next_task(tick_, poll_global)) {
        if (refresh_io_before_execute())
          break;
        idle_turns_ = 0;
        execute(*task);
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
        execution_.native_progress();
#endif
        continue;
      }
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
      if (shared_.external_host().has_ready(worker_id_)) {
        if (refresh_io_before_execute())
          break;
        execution_.execute(shared_.external_host(), scheduler_, worker_id_);
        idle_turns_ = 0;
        continue;
      }
#endif
      if (drive_io_idle()) {
        idle_turns_ = 0;
        continue;
      }
      if (auto task = scheduler_.steal_task()) {
        if (refresh_io_before_execute())
          break;
        idle_turns_ = 0;
        execute(*task);
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
        execution_.native_progress();
#endif
        continue;
      }
      if (scheduler_.is_searching() && idle_turns_ < shared_.config()._idle_spin_count) {
        ++idle_turns_;
        util::cpu_relax();
        continue;
      }
      idle_turns_ = 0;
      frame_cache.cache().flush();
      sleep();
    }
    faio::log::logger()->debug("worker {} stop", worker_id_);
  }

 private:
  /** @brief 安装空的任务 TLS 上下文；task awaiter 在进入用户代码前继承或恢复自身属性。 */
  void execute(std::coroutine_handle<> task) {
    scheduler_.before_execute();
    auto poll_budget = scheduler_.begin_execution();
    ::faio::detail::scoped_task_context context{::faio::detail::task_context{}};
    task.resume();
  }

  /** @brief 检查额外的就绪来源。 */
  bool additional_ready() const noexcept {
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    return shared_.external_host().has_ready(worker_id_);
#else
    return false;
#endif
  }

  /**  @brief 周期驱动保留真实开始时间锚点，结果沿原批次交付协议返回。 */
  bool drive_io() {
    return io_budget_.drive_with_anchor([] { return std::chrono::steady_clock::now(); },
                                        [this] { return drive_io_impl(); });
  }

  /**  @brief 空队列轮询时实际驱动 I/O，不读取时钟，也不前移最近一次计时驱动的时间基准。 */
  bool drive_io_idle() {
    return io_budget_.drive_without_anchor([this] { return drive_io_impl(); });
  }

  /**  @brief local/steal 的共用恢复前检查；true 表示驱动后确认已经关闭。 */
  bool refresh_io_before_execute() {
    if (io_budget_.refresh_before_execute(
            shared_.config()._max_io_delay,
            [] { return std::chrono::steady_clock::now(); },
            [this] { (void)drive_io_impl(); })) {
      shutdown_ = shared_.scheduler().closed();
      return shutdown_;
    }
    return false;
  }

  /**  @brief 计时和非计时入口均复用原有驱动，不包装或改写 I/O 后端。 */
  bool drive_io_impl() {
    scheduler_.interrupt_execution_chain();
    const auto progressed = io_engine_.drive(scheduler_);
    if (progressed)
      scheduler_.flush_ready();
    return progressed;
  }

  /** @brief 登记休眠 → 重查就绪任务 → 底层等待；保持完整握手以避免丢失唤醒。 */
  void sleep() {
    scheduler_.interrupt_execution_chain();
    shutdown_ = shared_.scheduler().closed();
    if (shutdown_ || !scheduler_.prepare_sleep())
      return;
    while (!shared_.scheduler().closed()) {
      if (scheduler_.has_ready_task() || additional_ready()) {
        if (scheduler_.finish_sleep(additional_ready()))
          return;
        continue;
      }
      io_engine_.wait_and_drive(scheduler_);
      if (scheduler_.finish_sleep(additional_ready()))
        return;
    }
    shutdown_ = true;
  }

  shared& shared_;            /**< 借用先构造、后销毁的 runtime 共享组件。 */
  std::size_t worker_id_;     /**< 当前工作线程的稳定编号。 */
  io_engine io_engine_;       /**< 所属线程的 I/O 引擎及底层唤醒端。 */
  io_worker_waker io_waker_;  /**< 具体 IO 域唤醒端，寿命覆盖本地注册。 */
  local_scheduler scheduler_; /**< 拥有本地队列，借用 shared 的调度域。 */
  ::faio::detail::execution_thread_binding<> binding_;  /**< 稳定的通用线程绑定，借用上述成员。 */
  ::faio::detail::execution_thread_guard thread_guard_; /**< 成对安装和恢复唯一调度 TLS 入口。 */
  worker_io_budget io_budget_{
      std::chrono::steady_clock::
          now()}; /**< 单个工作线程私有的保守时间基准，不使用共享原子变量，也不记录待补偿的时间状态。 */
  std::uint32_t tick_{0}; /**< 事件循环轮次，用于 I/O 和全局队列公平性。 */
  bool shutdown_{false};  /**< 周期或入睡时刷新，热路径不反复读取关闭原子。 */
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  execution_driver execution_;
#endif
  bool global_check_pending_{}; /**< 已到期但尚未执行的全局队列公平性检查。 */
  std::uint32_t idle_turns_{0}; /**< 连续空闲轮询次数，有工作时归零。 */
};
}  // namespace faio::runtime::detail
#endif
