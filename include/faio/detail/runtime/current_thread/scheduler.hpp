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
// 只有驱动线程访问 local_ 和 incoming_；生产者持有 mutex_ 后才能访问 remote_。
// 通过交换队列批量接收跨线程任务，避免每次恢复任务都获取互斥锁。
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

  // 在生产者使用的同一把锁下登记休眠意图，随后重新检查任务组是否完成。
  bool prepare_sleep() {
    std::lock_guard lock(mutex_);
    if (has_ready())
      return false;
    // 原子读改写与完成发布者同步，即使发布者此前看到驱动器尚未休眠，
    // 此处获取完成状态后再次检查，也能避免丢失唤醒。
    sleeping_.exchange(true, std::memory_order_acq_rel);
    return true;
  }

  void finish_sleep() {
    std::lock_guard lock(mutex_);
    sleeping_.store(false, std::memory_order_release);
  }

  // 未被等待的阻塞任务可能只更新完成计数，而不向就绪队列加入协程。
  void notify_completion() noexcept {
    if (on_driver() || !sleeping_.exchange(false, std::memory_order_acq_rel))
      return;
    std::lock_guard lock(mutex_);
    if (waker_)
      waker_->wake_up();
  }

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
      sleeping_.store(false, std::memory_order_release);  // 合并下一次休眠前的重复唤醒。
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

  std::vector<std::coroutine_handle<>> local_ = std::vector<std::coroutine_handle<>>(64);
  std::size_t head_{};
  std::size_t size_{};
  std::deque<std::coroutine_handle<>> incoming_;
  std::mutex mutex_;
  std::deque<std::coroutine_handle<>> remote_;
  std::atomic<bool> remote_pending_{false};
  io_engine* waker_{};  // 所有结果发布者退出后才能清空。
  std::atomic<bool> sleeping_{false};
  bool closed_{};
};

static_assert(coroutine_scheduler<current_thread_scheduler>);
}  // namespace faio::runtime::detail
#endif
