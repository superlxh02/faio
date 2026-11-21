/**
 * @file faio/detail/sync/latch.hpp
 * @brief 一次性协程倒计数。
 */
#ifndef FAIO_DETAIL_SYNC_LATCH_HPP
#define FAIO_DETAIL_SYNC_LATCH_HPP

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/wait/queue.hpp"
#include <atomic>
#include <cstddef>
#include <mutex>
#include <stdexcept>

namespace faio::sync {
/** @brief 一次性倒计数；计数归零后，后续 wait 均立即完成。 */
template <class S = default_scheduler>
class basic_latch {
 public:
  /** @brief 构造协程倒计数闩。 */
  explicit basic_latch(std::ptrdiff_t count) : remaining_(count) {
    if (count < 0)
      throw std::invalid_argument("latch 计数不能为负");
  }

  /** @brief 禁止复制构造协程倒计数闩。 */
  basic_latch(const basic_latch&) = delete;

  /** @brief 禁止复制赋值。 */
  basic_latch& operator=(const basic_latch&) = delete;

  /** @brief 减少剩余计数，首次归零时向全部已登记等待者发布完成。 */
  void count_down(std::ptrdiff_t n = 1) {
    if (n < 0)
      throw std::invalid_argument("latch count_down 越界");
    auto count = remaining_.load(std::memory_order_relaxed);
    while (count > n) {
      if (remaining_.compare_exchange_weak(
              count, count - n, std::memory_order_acq_rel, std::memory_order_relaxed))
        return;
    }
    detail::basic_wait_node<S>* nodes{};
    {
      std::lock_guard lock(mutex_);
      count = remaining_.load(std::memory_order_relaxed);
      for (;;) {
        if (n > count)
          throw std::invalid_argument("latch count_down 越界");
        if (remaining_.compare_exchange_weak(
                count, count - n, std::memory_order_acq_rel, std::memory_order_relaxed)) {
          if (count == n)
            nodes = waiters_.take_all();
          break;
        }
      }
    }
    detail::wake_all(nodes);
  }

  /** @brief 无阻塞地检查一次性计数是否已经归零。 */
  bool try_wait() const {
    std::lock_guard lock(mutex_);
    return remaining_.load(std::memory_order_acquire) == 0;
  }

  /** @brief 倒计数等待器。 */
  struct awaiter {
    basic_latch& self; /**< 借用所属倒计数对象；该对象在回调或等待期间不得移动或销毁。 */
    detail::basic_wait_node<S> node; /**< 内嵌的倒计数等待节点，从入队到完成交接期间不得移动。 */

    /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
    bool await_ready() const { return self.try_wait(); }

    /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
    template <class Promise>
      requires detail::matching_wait_scheduler<S, Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      {
        std::lock_guard lock(self.mutex_);
        if (self.remaining_.load(std::memory_order_acquire) == 0)
          return false;
        node.capture(h);
        self.waiters_.push(&node);
      }
      node.register_stop(self.mutex_, self.waiters_);
      return node.try_suspend();
    }

    /** @brief 等待结束后检查取消状态。 */
    void await_resume() const {
      if (node.cancelled())
        throw operation_cancelled{};
    }
  };

  /** @brief 创建等待计数归零的可取消等待器。 */
  awaiter wait() noexcept { return {*this, {}}; }

 private:
  mutable std::mutex mutex_;              /**< 串行保护倒计数状态；调度和用户回调须在锁外执行。 */
  std::atomic<std::ptrdiff_t> remaining_; /**< 尚未归还的完成票数，归零后永久保持等待条件满足。 */
  detail::basic_wait_queue<S>
      waiters_; /**< 不拥有节点的侵入式等待队列，节点由各等待者的协程帧保存。 */
};

using latch = basic_latch<>;
}  // namespace faio::sync
#endif
