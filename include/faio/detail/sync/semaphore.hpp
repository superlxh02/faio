#ifndef FAIO_DETAIL_SYNC_SEMAPHORE_HPP
#define FAIO_DETAIL_SYNC_SEMAPHORE_HPP

#include "faio/detail/coroutine/coroutine_wait.hpp"
#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace faio::sync {
// 无竞争时只做一次 CAS；有竞争时在短临界区内把 permit 直接交给最早等待者。
class semaphore {
 public:
  explicit semaphore(std::ptrdiff_t permits = 0) : permits_(permits) {
    if (permits < 0)
      throw std::invalid_argument("semaphore 初始令牌不能为负");
  }

  semaphore(const semaphore&) = delete;

  semaphore& operator=(const semaphore&) = delete;

  bool try_acquire() noexcept {
    auto count = permits_.load(std::memory_order_relaxed);
    while (count > 0) {
      if (permits_.compare_exchange_weak(
              count, count - 1, std::memory_order_acquire, std::memory_order_relaxed))
        return true;
    }
    return false;
  }

  struct acquire_awaiter {
    semaphore& sem;           // 借用等待操作所属的信号量，需存活到等待结束。
    detail::wait_node node;   // 保存在 awaiter/协程帧内的 FIFO 登记及取消节点。
    bool observe_stop{true};  // CV

    // 的取消清理阶段必须重新获取用户锁，不能被同一个停止令牌中断。
    bool await_ready() noexcept { return sem.try_acquire(); }

    bool await_suspend(std::coroutine_handle<> h) {
      {
        std::lock_guard lock(sem.mutex_);
        // 在队列锁内把零计数切换为 -1，阻止 release 的无锁快路径跳过等待者。
        // release 若先发布了令牌，则重新领取并立即继续；不会漏掉交接。
        for (;;) {
          if (sem.try_acquire())
            return false;
          std::ptrdiff_t empty = 0;
          if (sem.permits_.compare_exchange_strong(
                  empty, -1, std::memory_order_acq_rel, std::memory_order_relaxed)
              || empty == -1)
            break;
        }
        node.capture(h);
        sem.waiters_.push(&node);
      }
      if (observe_stop)
        node.register_stop(sem.mutex_, sem.waiters_);
      return node.arm();
    }

    void await_resume() const {
      if (node.cancelled())
        throw operation_cancelled{};
    }
  };

  acquire_awaiter acquire() noexcept { return {*this, {}, true}; }

  /** @brief 清理阶段的不取消等待，调用者保证归还许可；用于 CV 恢复用户 mutex。
   */
  acquire_awaiter acquire_uncancellable() noexcept { return {*this, {}, false}; }

  // permit 的析构自动归还令牌，适合跨 co_await 的临界区。
  class permit {
   public:
    explicit permit(semaphore& owner) noexcept : owner_(&owner) {}

    ~permit() {
      if (owner_)
        owner_->release();
    }

    permit(const permit&) = delete;

    permit& operator=(const permit&) = delete;

    permit(permit&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}

   private:
    semaphore* owner_;  // 借用归还目标；移动后源 permit 不再归还。
  };

  struct permit_awaiter {
    semaphore& sem;         // 成功后绑定 RAII permit 的归还目标。
    acquire_awaiter inner;  // 复用同一登记、唤醒与取消协议。

    bool await_ready() noexcept { return inner.await_ready(); }

    bool await_suspend(std::coroutine_handle<> h) { return inner.await_suspend(h); }

    permit await_resume() {
      inner.await_resume();
      return permit{sem};
    }
  };

  // guard 本身在调用者帧内构造；无竞争时不再额外分配 task 协程帧。
  permit_awaiter acquire_permit() noexcept { return {*this, acquire()}; }

  void release(std::ptrdiff_t count = 1) {
    if (count < 0)
      throw std::invalid_argument("semaphore release 不能为负");
    for (std::ptrdiff_t i = 0; i < count; ++i) {
      auto available = permits_.load(std::memory_order_relaxed);
      // 非负值保证等待链表为空；CAS 与登记方的 0→-1 竞争建立唯一线性化点。
      // 没有等待者时 release 只做一次原子修改，不再每次获取队列互斥锁。
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
      detail::wait_node* node;
      {
        std::lock_guard lock(mutex_);
        node = waiters_.pop();
        if (!node) {
          // 所有等待者可能已取消。此时恢复非负计数并发布这一枚新令牌。
          // 另一个 release 可能已恢复计数，因此不能无条件把计数写成 1。
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
          // 本次令牌直接给摘除者；最后一位离队后允许未来 release 走快路径。
          permits_.store(0, std::memory_order_release);
        }
      }
      if (node)
        node->wake();
    }
  }

  std::ptrdiff_t available_permits() const noexcept {
    const auto count = permits_.load(std::memory_order_acquire);
    return count < 0 ? 0 : count;
  }

 private:
  std::atomic<std::ptrdiff_t> permits_;  // 非负是可用令牌数，-1 表示进入队列交接模式。
  std::mutex mutex_;                     // 只保护有争用的登记、取消和 FIFO 交接。
  detail::wait_queue waiters_;           // 借用等待协程帧中的节点；有节点时计数保持 -1。
};
}  // namespace faio::sync
#endif
