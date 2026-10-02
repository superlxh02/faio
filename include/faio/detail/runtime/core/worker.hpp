#ifndef FAIO_DETAIL_RUNTIME_CORE_WORKER_HPP
#define FAIO_DETAIL_RUNTIME_CORE_WORKER_HPP

#include "faio/detail/coroutine/task_context.hpp"
#include "faio/detail/common/util/cpu_relax.hpp"
#include "faio/detail/runtime/core/io_engine.hpp"
#include "faio/detail/runtime/core/scheduler/local_scheduler.hpp"
#include "faio/detail/runtime/core/shared.hpp"
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
  worker(shared& shared_state, std::size_t worker_id)
      : shared_(shared_state), worker_id_(worker_id), io_engine_(shared_state.config()),
        scheduler_(shared_state.scheduler(), worker_id,
                   shared_state.config()._global_queue_interval, worker_waker_ref{io_engine_}),
        binding_(shared_state.scheduler_reference(), shared_state.task_lifetime_reference(),
                 scheduler_.local_state(), worker_id), thread_guard_(binding_) {}
  worker(const worker&) = delete;
  worker& operator=(const worker&) = delete;
  // 先确保所有事件循环都停止，再按成员逆序撤销绑定、注销队列并销毁 I/O。
  ~worker() { shared_.synchronize_worker_exit(); }

  // 驱动就绪协程与 I/O；调度算法由 local_scheduler 负责。
  void run() {
    faio::log::logger()->debug("worker {} started", worker_id_);
    while (!shutdown_) {
      ++tick_;
      // 持续有协程工作时仍定期轮询 I/O，避免完成事件长期得不到处理。
      if (tick_ % shared_.config()._io_interval == 0) {
        drive_io();
        shutdown_ = shared_.scheduler().closed();
        if (shutdown_) break;
      }
      if (auto task = scheduler_.next_task(tick_)) { idle_turns_ = 0; execute(*task); continue; }
      if (auto task = scheduler_.steal_task()) { idle_turns_ = 0; execute(*task); continue; }
      if (drive_io()) { idle_turns_ = 0; continue; }
      // 有界轮询减少短突发反复进入内核等待；名额一直计入共享搜索状态。
      if (scheduler_.is_searching() && idle_turns_ < shared_.config()._idle_spin_count) {
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
    auto* previous_tracker = ::faio::detail::current_tracker;
    auto previous_stop = ::faio::detail::current_stop_token;
    ::faio::detail::current_tracker = nullptr;
    ::faio::detail::current_stop_token = {};
    task.resume();
    ::faio::detail::current_tracker = previous_tracker;
    ::faio::detail::current_stop_token = std::move(previous_stop);
  }
  // 完成处理通过模板就绪接口交给调度器，一批事件仅做一次唤醒判断。
  bool drive_io() {
    const auto progressed = io_engine_.drive(scheduler_);
    if (progressed) scheduler_.flush_ready();
    return progressed;
  }
  // 登记休眠 → 重查就绪任务 → 底层等待；保持完整握手以避免丢失唤醒。
  void sleep() {
    shutdown_ = shared_.scheduler().closed();
    if (shutdown_ || !scheduler_.prepare_sleep()) return;
    while (!shared_.scheduler().closed()) {
      if (scheduler_.has_ready_task()) { (void)scheduler_.finish_sleep(); return; }
      io_engine_.wait_and_drive(scheduler_);
      scheduler_.flush_ready();
      if (scheduler_.finish_sleep()) return;
    }
    shutdown_ = true;
  }

  shared& shared_;                               // 借用先构造、后销毁的 runtime 共享组件。
  std::size_t worker_id_;                        // 当前工作线程的稳定编号。
  io_engine io_engine_;                         // 所属线程的 I/O 引擎及底层唤醒端。
  local_scheduler scheduler_;                   // 拥有本地队列，借用 shared 的调度域。
  ::faio::detail::execution_thread_binding binding_; // 稳定的通用线程绑定，借用上述成员。
  ::faio::detail::execution_thread_guard thread_guard_; // 成对安装和恢复唯一调度 TLS 入口。
  std::uint32_t tick_{0};                        // 事件循环轮次，用于 I/O 和全局队列公平性。
  bool shutdown_{false};                        // 周期或入睡时刷新，热路径不反复读取关闭原子。
  std::uint32_t idle_turns_{0};                  // 连续空闲轮询次数，有工作时归零。
};
} // namespace faio::runtime::detail
#endif // FAIO_DETAIL_RUNTIME_CORE_WORKER_HPP
