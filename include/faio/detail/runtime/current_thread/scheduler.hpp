/**
 * @file faio/detail/runtime/current_thread/scheduler.hpp
 * @brief 当前线程模式的本地就绪消费与远程批次转移。
 */
#ifndef FAIO_DETAIL_RUNTIME_CURRENT_THREAD_SCHEDULER_HPP
#define FAIO_DETAIL_RUNTIME_CURRENT_THREAD_SCHEDULER_HPP
#include "faio/detail/runtime/multi_thread/scheduler/local_scheduler.hpp"
#include <algorithm>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <mutex>
#include <optional>

namespace faio::runtime::detail {
using current_thread_scheduler = domain_scheduler;

/** @brief 持有驱动锁的唯一消费者，借用共享调度域提供的队列和发布协议。 */
class current_thread_owner_scheduler {
 public:
  /**
   * @brief 普通轮次直接领取本地任务，本地为空时转移一批远程任务。
   * @details 到期的远程检查、定向任务和耗尽的协作预算由既定调度策略处理。
   */
  static std::optional<std::coroutine_handle<>> next(local_scheduler& local, bool check_remote) {
    assert(local.domain_.num_workers() == 1);
    if (check_remote || local.private_global_check_pending_ || !local.private_queue_.empty()
        || local.execution_budget_.exhausted())
      return local.next_owner_task(check_remote);

    auto task = local.queue_.try_pop_local<true>();
    local.last_task_from_fast_ = task && local.queue_.last_pop_from_fast();
    if (task)
      return task;

    local.interrupt_execution_chain();
    return import_batch(local.domain_.global_queue(), local.queue_);
  }

 private:
  /**
   * @brief 将远程队列的批次直接写入空的本地环形队列，返回末项用于本轮执行。
   * @details 远程发布和关闭仍由同一把短锁串行保护。驱动线程独占本地写入，释放远程锁后
   *          一次发布环形队列尾部；维持返回项选择、批次上限和本地 FIFO 顺序的既定规则。
   */
  static std::optional<std::coroutine_handle<>> import_batch(
      global_ready_queue& remote, domain_scheduler::local_queue_type& local) {
    if (remote.empty())
      return std::nullopt;

    assert(local.empty_local());
    auto tail = local.tail_.load(std::memory_order_relaxed);
    std::coroutine_handle<> immediate;
    std::size_t count;
    {
      std::lock_guard lock(remote.mutex_);
      count = std::min(remote.queue_.size(), local.capacity() / 2);
      if (count == 0)
        return std::nullopt;
      for (std::size_t i = 1; i < count; ++i) {
        local.tasks_[tail++ & local.mask] = remote.queue_.front();
        remote.queue_.pop_front();
      }
      immediate = remote.queue_.front();
      remote.queue_.pop_front();
      if (remote.queue_.empty())
        remote.has_tasks_.store(false, std::memory_order_release);
    }
    if (count > 1) {
      local.tail_.store(tail, std::memory_order_release);
      local.fifo_hint_ = true;
    }
    return immediate;
  }
};
}  // namespace faio::runtime::detail
#endif
