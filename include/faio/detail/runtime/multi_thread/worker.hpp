/**
 * @file faio/detail/runtime/multi_thread/worker.hpp
 * @brief 运行时调度、线程协调与定时驱动：工作线程事件循环。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_WORKER_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_WORKER_HPP

#include "faio/detail/common/util/cpu_relax.hpp"
#include "faio/detail/coroutine/frame_allocator.hpp"
#include "faio/detail/coroutine/task_context.hpp"
#include "faio/detail/runtime/common/io_engine.hpp"
#include "faio/detail/runtime/common/worker_io_budget.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/local_scheduler.hpp"
#include "faio/detail/runtime/multi_thread/shared.hpp"
#include "faio/log.hpp"
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace faio::runtime::detail {
/// @brief 工作线程的事件循环：组合本地调度与 I/O，不再直接维护调度队列或窃取算法。
class worker {
 public:
  /// @brief I/O 引擎先构造，再登记本地调度器，保证被注册的唤醒端始终有效。
  worker(shared& shared_state, std::size_t worker_id)
      : shared_(shared_state),
        worker_id_(worker_id),
        io_engine_(shared_state.config(), true, shared_state.io_services()),
        scheduler_(shared_state.scheduler(),
                   worker_id,
                   shared_state.config()._global_queue_interval,
                   worker_waker_ref{io_engine_}),
        binding_(shared_state.scheduler_reference(),
                 shared_state.task_lifetime_reference(),
                 scheduler_.local_state(),
                 worker_id,
                 &shared_state.blocking(),
                 &shared_state.external_host()),
        thread_guard_(binding_) {
    /// @brief 把已构造完成的本线程 IO 域登记到运行时共享服务，保证后续取消与关闭可找到该域。
    shared_.register_io(worker_id_, io_engine_.context());
  }

  worker(const worker&) = delete;

  worker& operator=(const worker&) = delete;

  /// @brief 先确保所有事件循环都停止，再按成员逆序撤销绑定、注销队列并销毁 I/O。
  ~worker() { shared_.synchronize_worker_exit(); }

  /// @brief 驱动就绪协程与 I/O；调度算法由 local_scheduler 负责。
  /// @details 每轮按周期驱动 IO、处理外部节点或领取就绪协程；本域没有进展后
  /// 才尝试窃取。持续就绪期间在恢复边界检查 IO 延迟预算；真正入睡前先登记
  /// 休眠再复查所有就绪来源，防止工作发布与内核等待之间丢失唤醒。
  void run() {
    /// @brief 运行栈拥有 空闲块，退出先撤去 TLS 借用。
    ::faio::detail::coroutine_frame_cache_scope
        frame_cache;
    /// @brief 记录当前运行或轮层级的诊断信息；任务执行权、节点所有权与时间坐标由相邻状态操作维护。
    faio::log::logger()->debug("worker {} started", worker_id_);
    /// @brief 只在既有关闭握手确认后退出循环；每轮在协程、外部节点与 IO 之间推进。
    while (!shutdown_) {
      /// @brief 推进调度轮次，为 IO 驱动与全局队列公平性检查提供周期基准。
      ++tick_;
      /// @brief 记住已经到期的全局公平性检查，避免外部任务分支跳过检查后永久推迟它。
      global_check_pending_ = global_check_pending_
                              || tick_ % shared_.config()._global_queue_interval == 0;
      /// @brief 持续有协程工作时仍定期轮询 I/O，避免完成事件长期得不到处理。
      if (tick_ % shared_.config()._io_interval == 0) {
        /// @brief 实际推进后端事件与定时器，并中断连续快速槽执行链以重新建立协作预算。
        drive_io();
        /// @brief 在驱动或等待边界刷新关闭状态，避免每次恢复任务都反复读取关闭原子。
        shutdown_ = shared_.scheduler().closed();
        /// @brief 周期 IO 驱动后再次确认调度域关闭，防止在已关闭服务上继续恢复新任务。
        if (shutdown_)
          /// @brief 驱动后已确认关闭，结束当前执行循环。
          break;
      }
      /// @brief 轮到外部任务且目标队列非空时，先给受控任务图一次执行机会。
      if (prefer_external_ && shared_.external_host().has_ready(worker_id_)) {
        /// @brief 恢复已选任务前检查 IO 时间预算；触发驱动后若已关闭，则结束执行循环。
        if (refresh_io_before_execute())
          /// @brief 驱动后已确认关闭，结束当前执行循环。
          break;
        /// @brief 通过宿主分派守卫执行外部节点，节点回调可同步销毁其所属操作。
        execute_external();
        /// @brief 本次已给外部任务执行机会，下轮优先尝试协程任务以维持两类工作的公平性。
        prefer_external_ = false;
        /// @brief 发现实际工作后清空连续空闲自旋次数，下一次空闲重新计算等待预算。
        idle_turns_ = 0;
        /// @brief 结束当前调度轮，重新按就绪来源、公平性与 IO 预算选择后续工作。
        continue;
      }
      /// @brief 取走本轮待执行的全局公平性检查责任，下一次周期到期再重新登记。
      const auto poll_global = std::exchange(global_check_pending_, false);
      /// @brief 按全局公平性与本地快速槽规则领取一条任务；此时尚未恢复用户协程。
      if (auto task = scheduler_.next_task(tick_, poll_global)) {
        /// @brief 持续就绪逐恢复检查 elapsed；空闲驱动保留较早锚点，只会提前触发预算。
        /// 驱动仍在选中任务以后中断 fast-chain，并在恢复以前重查关闭状态。
        if (refresh_io_before_execute())
          /// @brief 驱动后已确认关闭，结束当前执行循环。
          break;
        /// @brief 发现实际工作后清空连续空闲自旋次数，下一次空闲重新计算等待预算。
        idle_turns_ = 0;
        /// @brief 安装本次执行上下文与协作预算后恢复已领取的协程句柄。
        execute(*task);
        /// @brief 本次已执行协程任务，下轮允许优先消费外部任务图节点。
        prefer_external_ = true;
        /// @brief 结束当前调度轮，重新按就绪来源、公平性与 IO 预算选择后续工作。
        continue;
      }
      /// @brief 协程队列暂空时再次检查外部节点，避免因优先级轮换推迟已有外部工作。
      if (shared_.external_host().has_ready(worker_id_)) {
        /// @brief 恢复已选任务前检查 IO 时间预算；触发驱动后若已关闭，则结束执行循环。
        if (refresh_io_before_execute())
          /// @brief 驱动后已确认关闭，结束当前执行循环。
          break;
        /// @brief 通过宿主分派守卫执行外部节点，节点回调可同步销毁其所属操作。
        execute_external();
        /// @brief 本次已给外部任务执行机会，下轮优先尝试协程任务以维持两类工作的公平性。
        prefer_external_ = false;
        /// @brief 发现实际工作后清空连续空闲自旋次数，下一次空闲重新计算等待预算。
        idle_turns_ = 0;
        /// @brief 结束当前调度轮，重新按就绪来源、公平性与 IO 预算选择后续工作。
        continue;
      }
      /// @brief 本地队列耗尽时先消费本域完成，避免不断窃取别域任务而推迟自己的IO。
      /// 完成仍由现有有界drive发布；没有本域进展时才进入原窃取/休眠握手。
      if (drive_io_idle()) {
        /// @brief 真正空闲驱动不读 clock，不前移保守旧锚点。
        idle_turns_ = 0;
        /// @brief 结束当前调度轮，重新按就绪来源、公平性与 IO 预算选择后续工作。
        continue;
      }
      /// @brief 本域没有新进展时才尝试远端窃取或全局回退，减少推迟本域完成交付。
      if (auto task = scheduler_.steal_task()) {
        /// @brief steal 同样检查旧锚点预算，不能用未计时 poll 前移恢复时间界。
        if (refresh_io_before_execute())
          /// @brief 驱动后已确认关闭，结束当前执行循环。
          break;
        /// @brief 发现实际工作后清空连续空闲自旋次数，下一次空闲重新计算等待预算。
        idle_turns_ = 0;
        /// @brief 安装本次执行上下文与协作预算后恢复已领取的协程句柄。
        execute(*task);
        /// @brief 本次已执行协程任务，下轮允许优先消费外部任务图节点。
        prefer_external_ = true;
        /// @brief 结束当前调度轮，重新按就绪来源、公平性与 IO 预算选择后续工作。
        continue;
      }
      /// @brief 有界轮询减少短突发反复进入内核等待；名额一直计入共享搜索状态。
      if (scheduler_.is_searching() && idle_turns_ < shared_.config()._idle_spin_count) {
        /// @brief 累计连续空闲轮次，使忙等最终有界结束。
        ++idle_turns_;
        /// @brief 发出平台自旋提示，降低短暂等待期间的执行资源争用。
        util::cpu_relax();
        /// @brief 结束当前调度轮，重新按就绪来源、公平性与 IO 预算选择后续工作。
        continue;
      }
      /// @brief 发现实际工作后清空连续空闲自旋次数，下一次空闲重新计算等待预算。
      idle_turns_ = 0;
      /// @brief 登记休眠、重新检查任务，再进入可被后端控制通知打断的内核等待。
      sleep();
    }
    /// @brief 记录当前运行或轮层级的诊断信息；任务执行权、节点所有权与时间坐标由相邻状态操作维护。
    faio::log::logger()->debug("worker {} stop", worker_id_);
  }

 private:
  /// @brief 安装干净的任务 TLS；task awaiter 会在进入用户代码前继承或恢复自己的属性。
  void execute(std::coroutine_handle<> task) {
    /// @brief 执行用户代码前归还搜索名额，使共享计数只统计真正正在搜索的线程。
    scheduler_.before_execute();
    /// @brief FIFO/global/steal补64，连续私有fast共用余额。
    auto poll_budget =
        scheduler_.begin_execution();
    /// @brief 先安装干净任务上下文；真正用户 task 的等待器在进入其代码前恢复自己的属性。
    ::faio::detail::scoped_task_context context{::faio::detail::task_context{}};
    /// @brief 恢复已独占领取的帧；挂起期间注册的后续恢复责任由相应等待协议接管。
    task.resume();
  }

  /// @brief 通过独立分派守卫执行外部节点，回调可同步销毁节点所属对象。
  void execute_external() noexcept {
    /// @brief 领取外部节点及独立分派守卫，守卫覆盖节点回调可能销毁操作对象的整个过程。
    auto dispatch = shared_.external_host().try_pop_external(worker_id_);
    /// @brief 其他执行路径已经取空目标队列时直接返回，不凭提示假定一定存在节点。
    if (!dispatch)
      return;
    /// @brief 执行用户代码前归还搜索名额，使共享计数只统计真正正在搜索的线程。
    scheduler_.before_execute();
    /// @brief 外部节点或真实 IO 驱动不属于连续私有快速任务，不能借用此前剩余预算。
    scheduler_.interrupt_execution_chain();
    /// @brief 为外部节点执行建立新的协作轮询预算，并在退出时恢复外层预算。
    ::faio::detail::cooperative_poll_scope budget;
    ::faio::detail::scoped_task_context context{{
        .stop_token = dispatch.root() ? dispatch.root()->stop_token() : std::stop_token{},
        .external_scope = dispatch.scope()}};
    /// @brief 借用当前已独占领取的外部节点，实际存活责任由分派守卫维持。
    auto* node = dispatch.node();
    /// @brief 执行前复制静态回调，回调内节点所属操作可能同步自销毁。
    auto execute = node->execute;
    /// @brief node 可在 receiver 内销毁；guard 保留独立 root/dispatch 责任。
    execute(node);
  }

  /** @brief 周期驱动保留真实开始时间锚点，结果沿原批次交付协议返回。 */
  bool drive_io() {
    return io_budget_.drive_with_anchor([] { return std::chrono::steady_clock::now(); },
                                        [this] { return drive_io_impl(); });
  }

  /** @brief 空队列轮询真正驱动 IO，不读 clock，也不前移最近计时驱动的锚点。 */
  bool drive_io_idle() {
    return io_budget_.drive_without_anchor([this] { return drive_io_impl(); });
  }

  /** @brief local/steal 的共用恢复前检查；true 表示驱动后确认已经关闭。 */
  bool refresh_io_before_execute() {
    if (io_budget_.refresh_before_execute(
            shared_.config()._max_io_delay,
            [] { return std::chrono::steady_clock::now(); },
            [this] { (void)drive_io_impl(); })) {
      /// @brief 选中任务以后驱动仍必须重查关闭。
      shutdown_ = shared_.scheduler().closed();
      return shutdown_;
    }
    return false;
  }

  /** @brief 所有计时/未计时入口只复用原真实驱动；不包装或改写 IO backend。 */
  bool drive_io_impl() {
    /// @brief 已选中的fast任务也不能继续借用驱动前的预算。
    scheduler_.interrupt_execution_chain();
    /// @brief 原SQE/CQE/readiness/计时器路径。
    const auto progressed = io_engine_.drive(scheduler_);
    /// @brief 原批次通知/自然唤醒协议不变。
    if (progressed)
      scheduler_.flush_ready();
    return progressed;
  }

  /// @brief 登记休眠 → 重查就绪任务 → 底层等待；保持完整握手以避免丢失唤醒。
  void sleep() {
    /// @brief 真正等待/早期ready握手都不是连续fast执行。
    scheduler_.interrupt_execution_chain();
    /// @brief 在驱动或等待边界刷新关闭状态，避免每次恢复任务都反复读取关闭原子。
    shutdown_ = shared_.scheduler().closed();
    /// @brief 已关闭或登记休眠前又发现本地任务时，不进入内核等待。
    if (shutdown_ || !scheduler_.prepare_sleep())
      return;
    /// @brief 仅在域仍开放时继续等待；每次内核返回都重新完成休眠状态握手。
    while (!shared_.scheduler().closed()) {
      /// @brief 休眠登记后复查所有可执行来源，堵住工作发布与真正入睡之间的丢失唤醒窗口。
      if (scheduler_.has_ready_task() || shared_.external_host().has_ready(worker_id_)) {
        /// @brief 先从休眠集合撤销当前线程，再按实际工作状态恢复共享搜索计数及批次通知责任。
        (void)scheduler_.finish_sleep(shared_.external_host().has_ready(worker_id_));
        return;
      }
      /// @brief 进入可被控制通知打断的后端等待，返回后将 IO 与定时完成交给本地就绪接收端。
      io_engine_.wait_and_drive(scheduler_);
      /// @brief finish_sleep 先撤销自身休眠记录再通知批次，避免从 sleepers
      /// 末尾唤醒自己。
      if (scheduler_.finish_sleep(shared_.external_host().has_ready(worker_id_)))
        return;
    }
    /// @brief 共享调度域已关闭，记录私有退出状态使工作线程主循环结束。
    shutdown_ = true;
  }

  shared& shared_;  ///< 借用先构造、后销毁的 runtime 共享组件。
  std::size_t worker_id_;  ///< 当前工作线程的稳定编号。
  io_engine io_engine_;  ///< 所属线程的 I/O 引擎及底层唤醒端。
  local_scheduler scheduler_;  ///< 拥有本地队列，借用 shared 的调度域。
  ::faio::detail::execution_thread_binding binding_;  ///< 稳定的通用线程绑定，借用上述成员。
  ::faio::detail::execution_thread_guard thread_guard_;  ///< 成对安装和恢复唯一调度 TLS 入口。
  worker_io_budget io_budget_{
      std::chrono::steady_clock::now()};  ///< 单 worker 私有保守时间锚点，无共享原子或债务状态。
  std::uint32_t tick_{0};  ///< 事件循环轮次，用于 I/O 和全局队列公平性。
  bool shutdown_{false};  ///< 周期或入睡时刷新，热路径不反复读取关闭原子。
  bool prefer_external_{true};  ///< 下一轮是否优先领取外部任务，使外部与协程任务交替获得机会。
  bool global_check_pending_{};  ///< 已到期但尚未执行的全局队列公平性检查。
  std::uint32_t idle_turns_{0};  ///< 连续空闲轮询次数，有工作时归零。
};
}
#endif
