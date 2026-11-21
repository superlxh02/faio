/**
 * @file faio/detail/sync/semaphore.hpp
 * @brief 计数信号量与许可守卫。
 */
#ifndef FAIO_DETAIL_SYNC_SEMAPHORE_HPP
#define FAIO_DETAIL_SYNC_SEMAPHORE_HPP

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/wait/queue.hpp"
#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace faio::sync {
/** @brief 无竞争时只执行一次 CAS；有竞争时，在短临界区内将许可直接交给最早的等待者。 */
template <class S = default_scheduler>
class basic_semaphore {
 public:
  /** @brief 构造协程信号量。 */
  explicit basic_semaphore(std::ptrdiff_t permits = 0) : permits_(permits) {
    if (permits < 0)
      throw std::invalid_argument("semaphore 初始令牌不能为负");
  }

  /** @brief 禁止复制构造协程信号量。 */
  basic_semaphore(const basic_semaphore&) = delete;

  /** @brief 禁止复制赋值。 */
  basic_semaphore& operator=(const basic_semaphore&) = delete;

  /** @brief 尝试立即取得许可，已有等待者时遵循原语的公平性约定。 */
  bool try_acquire() noexcept {
    auto count = permits_.load(std::memory_order_relaxed);
    while (count > 0) {
      if (permits_.compare_exchange_weak(
              count, count - 1, std::memory_order_acquire, std::memory_order_relaxed))
        return true;
    }
    return false;
  }

  /** @brief 信号量许可等待器。 */
  struct acquire_awaiter {
    basic_semaphore& sem;            /**< 借用等待操作所属的信号量，需存活到等待结束。 */
    detail::basic_wait_node<S> node; /**< 保存在 awaiter/协程帧内的 FIFO 登记及取消节点。 */
    bool observe_stop{true}; /**< 是否订阅取消；条件变量清理时须不可取消地重新取得用户锁。 */

    /** @brief 先尝试立即领取许可，成功时无需登记等待节点。 */
    bool await_ready() noexcept { return sem.try_acquire(); }

    /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
    template <class Promise>
      requires detail::matching_wait_scheduler<S, Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      node.capture(h);
      {
        std::lock_guard lock(sem.mutex_);
        for (;;) {
          if (sem.try_acquire())
            return false;
          std::ptrdiff_t empty = 0;
          if (sem.permits_.compare_exchange_strong(
                  empty, -1, std::memory_order_acq_rel, std::memory_order_relaxed)
              || empty == -1)
            break;
        }
        sem.waiters_.push(&node);
      }
      if (observe_stop)
        node.register_stop(sem.mutex_, sem.waiters_);
      return node.try_suspend();
    }

    /** @brief 等待结束后检查取消状态。 */
    void await_resume() const {
      if (node.cancelled())
        throw operation_cancelled{};
    }
  };

  /** @brief 创建异步许可等待器，等待期间通过节点协议响应通知与取消。 */
  acquire_awaiter acquire() noexcept { return {*this, {}, true}; }

  /**  @brief 清理阶段的不可取消等待，由调用方保证归还许可；用于条件变量重新取得用户 mutex。 */
  acquire_awaiter acquire_uncancellable() noexcept { return {*this, {}, false}; }

  /** @brief 析构时自动归还许可，适用于跨 co_await 的临界区。 */
  class permit {
   public:
    /** @brief 接管已取得许可的归还责任。 */
    explicit permit(basic_semaphore& owner) noexcept : owner_(&owner) {}

    /** @brief 析构时归还仍持有的许可。 */
    ~permit() {
      if (owner_)
        owner_->release();
    }

    /** @brief 禁止复制许可守卫。 */
    permit(const permit&) = delete;

    /** @brief 禁止复制赋值。 */
    permit& operator=(const permit&) = delete;

    /** @brief 转交许可的归还责任，并清空原守卫。 */
    permit(permit&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}

   private:
    basic_semaphore* owner_; /**< 借用归还目标；移动后源 permit 不再归还。 */
  };

  /** @brief 取得许可守卫的等待器。 */
  struct permit_awaiter {
    basic_semaphore& sem;  /**< 成功后绑定 RAII permit 的归还目标。 */
    acquire_awaiter inner; /**< 复用同一登记、唤醒与取消协议。 */

    /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
    bool await_ready() noexcept { return inner.await_ready(); }

    /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
    template <class Promise>
      requires detail::matching_wait_scheduler<S, Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      return inner.await_suspend(h);
    }

    /** @brief 取得持有许可的作用域守卫。 */
    permit await_resume() {
      inner.await_resume();
      return permit{sem};
    }
  };

  /** @brief 守卫直接构造在调用方的协程帧中；无竞争时无需额外分配 task 协程帧。 */
  permit_awaiter acquire_permit() noexcept { return {*this, acquire()}; }

  /** @brief 归还指定数量的许可；有等待者时直接按 FIFO 交接，否则发布为可用计数。 */
  void release(std::ptrdiff_t count = 1) {
    if (count < 0)
      throw std::invalid_argument("semaphore release 不能为负");
    for (std::ptrdiff_t i = 0; i < count; ++i) {
      auto available = permits_.load(std::memory_order_relaxed);
      bool published = false;
      while (available >= 0) {
        if (available == std::numeric_limits<std::ptrdiff_t>::max())
          throw std::overflow_error("semaphore 令牌计数溢出");
        if (permits_.compare_exchange_weak(
                available, available + 1, std::memory_order_release, std::memory_order_relaxed)) {
          published = true;
          break;
        }
      }
      if (published)
        continue;
      detail::basic_wait_node<S>* node;
      {
        std::lock_guard lock(mutex_);
        node = waiters_.pop();
        if (!node) {
          auto old = permits_.load(std::memory_order_relaxed);
          for (;;) {
            if (old == std::numeric_limits<std::ptrdiff_t>::max())
              throw std::overflow_error("semaphore 令牌计数溢出");
            const auto next = old == -1 ? 1 : old + 1;
            if (permits_.compare_exchange_weak(
                    old, next, std::memory_order_release, std::memory_order_relaxed))
              break;
          }
        } else if (!waiters_.head) {
          permits_.store(0, std::memory_order_release);
        }
      }
      if (node)
        node->wake();
    }
  }

  /** @brief 读取当前可用许可数。 */
  std::ptrdiff_t available_permits() const noexcept {
    const auto count = permits_.load(std::memory_order_acquire);
    return count < 0 ? 0 : count;
  }

 private:
  std::atomic<std::ptrdiff_t> permits_; /**< 非负是可用令牌数，-1 表示进入队列交接模式。 */
  std::mutex mutex_;                    /**< 只保护有争用的登记、取消和 FIFO 交接。 */
  detail::basic_wait_queue<S> waiters_; /**< 借用等待协程帧中的节点；有节点时计数保持 -1。 */
};

using semaphore = basic_semaphore<>;
}  // namespace faio::sync
#endif
