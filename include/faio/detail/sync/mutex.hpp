/**
 * @file faio/detail/sync/mutex.hpp
 * @brief 协程互斥锁与锁守卫。
 */
#ifndef FAIO_DETAIL_SYNC_MUTEX_HPP
#define FAIO_DETAIL_SYNC_MUTEX_HPP

#include "faio/detail/sync/semaphore.hpp"
#include <utility>

namespace faio::sync {
/** @brief 异步互斥锁。等待时挂起协程，工作线程可以继续执行；按 FIFO 交接，避免争用时插队。 */
template <class S = default_scheduler>
class basic_mutex {
 public:
  /** @brief 构造协程互斥锁。 */
  basic_mutex() : permit_(1) {}

  /** @brief 禁止复制构造协程互斥锁。 */
  basic_mutex(const basic_mutex&) = delete;

  /** @brief 禁止复制赋值。 */
  basic_mutex& operator=(const basic_mutex&) = delete;

  /** @brief 尝试取得互斥锁，不阻塞也不挂起调用协程。 */
  bool try_lock() noexcept { return permit_.try_acquire(); }

  /** @brief 创建异步互斥等待器，争用时挂起协程而非阻塞工作线程。 */
  auto lock() noexcept { return permit_.acquire(); }

  /**  @brief 条件变量重新加锁时使用的清理路径；收到停止请求后，仍须保证用户守卫持锁。 */
  auto lock_uncancellable() noexcept { return permit_.acquire_uncancellable(); }

  /** @brief 归还互斥许可，并按等待队列顺序向后继交接。 */
  void unlock() { permit_.release(); }

  /** @brief 互斥锁的作用域持有者。 */
  class guard {
   public:
    /** @brief 接管已取得互斥锁的归还责任。 */
    explicit guard(basic_mutex& m) noexcept : mutex_(&m) {}

    /** @brief 析构时归还仍持有的互斥锁。 */
    ~guard() {
      if (mutex_)
        mutex_->unlock();
    }

    /** @brief 禁止复制锁守卫。 */
    guard(const guard&) = delete;

    /** @brief 禁止复制赋值。 */
    guard& operator=(const guard&) = delete;

    /** @brief 转交锁的归还责任，并清空原守卫。 */
    guard(guard&& other) noexcept : mutex_(std::exchange(other.mutex_, nullptr)) {}

   private:
    basic_mutex* mutex_; /**< 借用已取得的互斥锁；守卫析构归还，移动后源守卫置空。 */
  };

  /** @brief 取得互斥锁并返回作用域守卫的等待器。 */
  struct guard_awaiter {
    basic_mutex& owner; /**< 借用所属互斥锁；锁在回调或等待期间不得移动或销毁。 */
    typename basic_semaphore<S>::acquire_awaiter inner; /**< 执行实际等待协议的内层等待器。 */

    /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
    bool await_ready() noexcept { return inner.await_ready(); }

    /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
    template <class Promise>
      requires detail::matching_wait_scheduler<S, Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      return inner.await_suspend(h);
    }

    /** @brief 取得已持锁的作用域守卫。 */
    guard await_resume() {
      inner.await_resume();
      return guard{owner};
    }
  };

  /** @brief 创建成功取得互斥锁后返回 RAII 守卫的等待器。 */
  guard_awaiter scoped_lock() noexcept { return {*this, permit_.acquire()}; }

 private:
  basic_semaphore<S> permit_; /**< 限制一次可同时持有多少资源的信号量。 */
};

using mutex = basic_mutex<>;
}  // namespace faio::sync
#endif
