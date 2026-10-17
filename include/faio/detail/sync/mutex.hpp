/**
 * @file faio/detail/sync/mutex.hpp
 * @brief 协程同步原语：协程互斥锁与锁守卫。
 * @details 同步等待保存帧内节点，队列只借用地址。登记、通知与取消争夺唯一恢复权；跨线程通知经保存的调度器恢复，用户协程不在工作线程上阻塞等待。
 */
#ifndef FAIO_DETAIL_SYNC_MUTEX_HPP
#define FAIO_DETAIL_SYNC_MUTEX_HPP

#include "faio/detail/sync/semaphore.hpp"
#include <utility>

namespace faio::sync {
/// @brief 异步互斥锁。等待者挂起协程而非阻塞 worker；FIFO 交接避免争用时的插队。
class mutex {
 public:
  mutex() : permit_(1) {}

  mutex(const mutex&) = delete;

  mutex& operator=(const mutex&) = delete;

  /// @brief 尝试取得互斥锁，不阻塞也不挂起调用协程。
  bool try_lock() noexcept { return permit_.try_acquire(); }

  /// @brief 创建异步互斥等待器，争用时挂起协程而非阻塞工作线程。
  auto lock() noexcept { return permit_.acquire(); }

  /** @brief 条件变量恢复锁的清理路径；停止请求不能使用户 guard 失去持锁不变量。
   */
  auto lock_uncancellable() noexcept { return permit_.acquire_uncancellable(); }

  /// @brief 归还互斥许可，并按等待队列顺序向后继交接。
  void unlock() { permit_.release(); }

  class guard {
   public:
    explicit guard(mutex& m) noexcept : mutex_(&m) {}

    ~guard() {
      /// @brief 只有仍持有归还责任的守卫在析构时解锁，移动后的空守卫保持无操作。
      if (mutex_)
        /// @brief 异常展开或正常退出都成对归还用户互斥许可，避免跨挂起临界区泄漏锁。
        mutex_->unlock();
    }

    guard(const guard&) = delete;

    guard& operator=(const guard&) = delete;

    guard(guard&& other) noexcept : mutex_(std::exchange(other.mutex_, nullptr)) {}

   private:
    mutex* mutex_;  ///< 借用已取得的互斥锁；守卫析构归还，移动后源守卫置空。
  };

  struct guard_awaiter {
    mutex& owner;  ///< 借用所属对象的稳定地址，回调或等待期间不得迁移或销毁。
    semaphore::acquire_awaiter inner;  ///< 执行实际等待协议的内层等待器。

    /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
    bool await_ready() noexcept { return inner.await_ready(); }

    /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
    bool await_suspend(std::coroutine_handle<> h) { return inner.await_suspend(h); }

    /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
    guard await_resume() {
      /// @brief 先让底层等待器检查失败或取消，成功后才能构造持有许可的守卫。
      inner.await_resume();
      /// @brief 互斥等待成功后返回锁守卫，使后续异常与 co_await 路径也能成对解锁。
      return guard{owner};
    }
  };

  /// @brief 创建成功取得互斥锁后返回 RAII 守卫的等待器。
  guard_awaiter scoped_lock() noexcept { return {*this, permit_.acquire()}; }

 private:
  semaphore permit_;  ///< 限制一次可同时持有多少资源的信号量。
};
}
#endif
