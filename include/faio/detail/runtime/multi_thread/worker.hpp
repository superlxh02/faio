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
// 工作线程的事件循环：组合本地调度与 I/O，不再直接维护调度队列或窃取算法。
class worker {
public:
  // I/O 引擎先构造，再登记本地调度器，保证被注册的唤醒端始终有效。
  worker(shared &shared_state, std::size_t worker_id)
      : shared_(shared_state), worker_id_(worker_id),
        io_engine_(shared_state.config(), true, shared_state.io_services()),
        scheduler_(shared_state.scheduler(), worker_id,
                   shared_state.config()._global_queue_interval,
                   worker_waker_ref{io_engine_}),
        binding_(shared_state.scheduler_reference(),
                 shared_state.task_lifetime_reference(),
                 scheduler_.local_state(), worker_id, &shared_state.blocking()),
        thread_guard_(binding_) {
    shared_.register_io(worker_id_, io_engine_.context());
  }
  worker(const worker &) = delete;
  worker &operator=(const worker &) = delete;
  // 先确保所有事件循环都停止，再按成员逆序撤销绑定、注销队列并销毁 I/O。
  ~worker() { shared_.synchronize_worker_exit(); }

  // 驱动就绪协程与 I/O；调度算法由 local_scheduler 负责。
  void run() {
    ::faio::detail::coroutine_frame_cache_scope
        frame_cache; // 运行栈 owns 空闲块，退出先撤去 TLS 借用。
    faio::log::logger()->debug("worker {} started", worker_id_);
    while (!shutdown_) {
      ++tick_;
      // 持续有协程工作时仍定期轮询 I/O，避免完成事件长期得不到处理。
      if (tick_ % shared_.config()._io_interval == 0) {
        drive_io();
        shutdown_ = shared_.scheduler().closed();
        if (shutdown_)
          break;
      }
      if (auto task = scheduler_.next_task(tick_)) {
        // 持续就绪逐恢复检查 elapsed；空闲驱动保留较早锚点，只会提前触发预算。
        // 驱动仍在选中任务以后中断 fast-chain，并在恢复以前重查关闭状态。
        if (refresh_io_before_execute())
          break;
        idle_turns_ = 0;
        execute(*task);
        continue;
      }
      // 本地队列耗尽时先消费本域完成，避免不断窃取别域任务而推迟自己的IO。
      // 完成仍由现有有界drive发布；没有本域进展时才进入原窃取/休眠握手。
      if (drive_io_idle()) { // 真正空闲驱动不读 clock，不前移保守旧锚点。
        idle_turns_ = 0;
        continue;
      }
      if (auto task = scheduler_.steal_task()) {
        // steal 同样检查旧锚点预算，不能用未计时 poll 前移恢复时间界。
        if (refresh_io_before_execute())
          break;
        idle_turns_ = 0;
        execute(*task);
        continue;
      }
      // 有界轮询减少短突发反复进入内核等待；名额一直计入共享搜索状态。
      if (scheduler_.is_searching() &&
          idle_turns_ < shared_.config()._idle_spin_count) {
        ++idle_turns_;
        util::cpu_relax();
        continue;
      }
      idle_turns_ = 0;
      sleep();
    }
    faio::log::logger()->debug("worker {} stop", worker_id_);
  }

private:
  // 安装干净的任务 TLS；task awaiter 会在进入用户代码前继承或恢复自己的属性。
  void execute(std::coroutine_handle<> task) {
    scheduler_.before_execute();
    auto poll_budget =
        scheduler_
            .begin_execution(); // FIFO/global/steal补64，连续私有fast共用余额。
    auto *previous_tracker = ::faio::detail::current_tracker;
    auto previous_stop = ::faio::detail::current_stop_token;
    ::faio::detail::current_tracker = nullptr;
    ::faio::detail::current_stop_token = {};
    task.resume();
    ::faio::detail::current_tracker = previous_tracker;
    ::faio::detail::current_stop_token = std::move(previous_stop);
  }
  /** @brief 周期驱动保留真实开始时间锚点，结果沿原批次交付协议返回。 */
  bool drive_io() {
    return io_budget_.drive_with_anchor(
        [] { return std::chrono::steady_clock::now(); },
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
      shutdown_ =
          shared_.scheduler().closed(); // 选中任务以后驱动仍必须重查关闭。
      return shutdown_;
    }
    return false;
  }
  /** @brief 所有计时/未计时入口只复用原真实驱动；不包装或改写 IO backend。 */
  bool drive_io_impl() {
    scheduler_
        .interrupt_execution_chain(); // 已选中的fast任务也不能继续借用驱动前的预算。
    const auto progressed =
        io_engine_.drive(scheduler_); // 原SQE/CQE/readiness/计时器路径。
    if (progressed)
      scheduler_.flush_ready(); // 原批次通知/自然唤醒协议不变。
    return progressed;
  }
  // 登记休眠 → 重查就绪任务 → 底层等待；保持完整握手以避免丢失唤醒。
  void sleep() {
    scheduler_
        .interrupt_execution_chain(); // 真正等待/早期ready握手都不是连续fast执行。
    shutdown_ = shared_.scheduler().closed();
    if (shutdown_ || !scheduler_.prepare_sleep())
      return;
    while (!shared_.scheduler().closed()) {
      if (scheduler_.has_ready_task()) {
        (void)scheduler_.finish_sleep();
        return;
      }
      io_engine_.wait_and_drive(scheduler_);
      // finish_sleep 先撤销自身休眠记录再通知批次，避免从 sleepers
      // 末尾唤醒自己。
      if (scheduler_.finish_sleep())
        return;
    }
    shutdown_ = true;
  }

  shared &shared_;            // 借用先构造、后销毁的 runtime 共享组件。
  std::size_t worker_id_;     // 当前工作线程的稳定编号。
  io_engine io_engine_;       // 所属线程的 I/O 引擎及底层唤醒端。
  local_scheduler scheduler_; // 拥有本地队列，借用 shared 的调度域。
  ::faio::detail::execution_thread_binding
      binding_; // 稳定的通用线程绑定，借用上述成员。
  ::faio::detail::execution_thread_guard
      thread_guard_; // 成对安装和恢复唯一调度 TLS 入口。
  worker_io_budget io_budget_{
      std::chrono::steady_clock::
          now()};         // 单 worker 私有保守时间锚点，无共享原子或债务状态。
  std::uint32_t tick_{0}; // 事件循环轮次，用于 I/O 和全局队列公平性。
  bool shutdown_{false};  // 周期或入睡时刷新，热路径不反复读取关闭原子。
  std::uint32_t idle_turns_{0}; // 连续空闲轮询次数，有工作时归零。
};
} // namespace faio::runtime::detail
#endif
