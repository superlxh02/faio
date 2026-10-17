/**
 * @file faio/detail/runtime/current_thread/scheduler.hpp
 * @brief 运行时调度、线程协调与定时驱动：调度接口。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#ifndef FAIO_DETAIL_RUNTIME_CURRENT_THREAD_SCHEDULER_HPP
#define FAIO_DETAIL_RUNTIME_CURRENT_THREAD_SCHEDULER_HPP

#include "faio/detail/coroutine/execution_thread.hpp"
#include "faio/detail/runtime/common/io_engine.hpp"
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace faio::runtime::detail {
/// @brief 只有驱动线程访问 local_ 和 incoming_；生产者持有 mutex_ 后才能访问 remote_。
/// 通过交换队列批量接收跨线程任务，避免每次恢复任务都获取互斥锁。
class current_thread_scheduler {
 public:
  void enqueue(std::coroutine_handle<> task) {
    if (on_driver()) {
      enqueue_ready(task);
      return;
    }
    std::lock_guard lock(mutex_);
    if (closed_)
      throw std::logic_error("单线程调度器已关闭");
    remote_.push_back(task);
    remote_pending_.store(true, std::memory_order_release);
    notify_locked();
  }

  void enqueue_ready(std::coroutine_handle<> task) {
    if (size_ == local_.size()) {
      std::vector<std::coroutine_handle<>> larger(local_.size() * 2);
      for (std::size_t i = 0; i < size_; ++i)
        larger[i] = local_[(head_ + i) & (local_.size() - 1)];
      local_.swap(larger);
      head_ = 0;
    }
    local_[(head_ + size_) & (local_.size() - 1)] = task;
    ++size_;
  }

  std::optional<std::coroutine_handle<>> next(bool check_remote) {
    if (check_remote)
      if (auto task = pop_remote())
        return task;
    if (size_ != 0) {
      const auto task = local_[head_];
      head_ = (head_ + 1) & (local_.size() - 1);
      --size_;
      return task;
    }
    return pop_remote();
  }

  bool has_ready() const noexcept {
    return size_ != 0 || !incoming_.empty() || remote_pending_.load(std::memory_order_acquire);
  }

  void set_waker(io_engine* engine) {
    std::lock_guard lock(mutex_);
    waker_ = engine;
  }

  /// @brief 在生产者使用的同一把锁下登记休眠意图，随后重新检查任务组是否完成。
  bool prepare_sleep() {
    std::lock_guard lock(mutex_);
    if (has_ready())
      return false;
    /// @brief 原子读改写与完成发布者同步，即使发布者此前看到驱动器尚未休眠，
    /// 此处获取完成状态后再次检查，也能避免丢失唤醒。
    sleeping_.exchange(true, std::memory_order_acq_rel);
    return true;
  }

  void finish_sleep() {
    std::lock_guard lock(mutex_);
    sleeping_.store(false, std::memory_order_release);
  }

  /// @brief 未被等待的阻塞任务可能只更新完成计数，而不向就绪队列加入协程。
  void notify_completion() noexcept {
    if (on_driver() || !sleeping_.exchange(false, std::memory_order_acq_rel))
      return;
    std::lock_guard lock(mutex_);
    if (waker_)
      waker_->wake_up();
  }

  /// @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。
  void close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    waker_ = nullptr;
  }

 private:
  bool on_driver() noexcept {
    const auto* binding = ::faio::detail::current_execution_thread;
    return binding && binding->local_state_for<current_thread_scheduler>(scheduler_ref{*this});
  }

  void notify_locked() noexcept {
    if (sleeping_.load(std::memory_order_relaxed) && waker_) {
      /// @brief 合并下一次休眠前的重复唤醒。
      sleeping_.store(false, std::memory_order_release);
      waker_->wake_up();
    }
  }

  std::optional<std::coroutine_handle<>> pop_remote() {
    if (incoming_.empty()) {
      if (!remote_pending_.load(std::memory_order_acquire))
        return std::nullopt;
      std::lock_guard lock(mutex_);
      incoming_.swap(remote_);
      remote_pending_.store(false, std::memory_order_release);
    }
    if (incoming_.empty())
      return std::nullopt;
    const auto task = incoming_.front();
    incoming_.pop_front();
    return task;
  }

  std::vector<std::coroutine_handle<>> local_ = std::vector<std::coroutine_handle<>>(64);  ///< 少量缓冲区向量使用的内联数组，避免普通请求堆分配。
  std::size_t head_{};  ///< 下一条待消费记录的索引。
  std::size_t size_{};  ///< 当前有效元素或缓冲字节的数量。
  std::deque<std::coroutine_handle<>> incoming_;  ///< 从远程队列批量取回、等待本线程消费的任务。
  std::mutex mutex_;  ///< 串行保护所属对象的共享状态；调度与用户回调应遵循该对象的锁外调用协议。
  std::deque<std::coroutine_handle<>> remote_;  ///< 跨线程提交的协程句柄 FIFO，由远程队列锁保护。
  std::atomic<bool> remote_pending_{false};  ///< 远程队列的保守非空提示，用于降低空轮询锁争用。
  io_engine* waker_{};  ///< 所有结果发布者退出后才能清空。
  std::atomic<bool> sleeping_{false};  ///< 当前线程是否已经登记内核等待，投递方据此承担唤醒责任。
  bool closed_{};  ///< 是否已关闭新任务入口；已接受任务仍按所属组件协议排空。
};

static_assert(coroutine_scheduler<current_thread_scheduler>);
}
#endif
