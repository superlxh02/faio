#ifndef FAIO_DETAIL_SYNC_MUTEX_HPP
#define FAIO_DETAIL_SYNC_MUTEX_HPP

#include "faio/detail/sync/semaphore.hpp"
#include <utility>

namespace faio::sync {
// 异步互斥锁。等待者挂起协程而非阻塞 worker；FIFO 交接避免争用时的插队。
class mutex {
public:
  mutex() : permit_(1) {}
  mutex(const mutex&) = delete;
  mutex& operator=(const mutex&) = delete;
  bool try_lock() noexcept { return permit_.try_acquire(); }
  auto lock() noexcept { return permit_.acquire(); }
  void unlock() { permit_.release(); }

  class guard {
  public:
    explicit guard(mutex& m) noexcept : mutex_(&m) {}
    ~guard() { if (mutex_) mutex_->unlock(); }
    guard(const guard&) = delete;
    guard& operator=(const guard&) = delete;
    guard(guard&& other) noexcept : mutex_(std::exchange(other.mutex_, nullptr)) {}
  private:
    mutex* mutex_;
  };
  struct guard_awaiter {
    mutex& owner;
    semaphore::acquire_awaiter inner;
    bool await_ready() noexcept { return inner.await_ready(); }
    bool await_suspend(std::coroutine_handle<> h) { return inner.await_suspend(h); }
    guard await_resume() {
      inner.await_resume();
      return guard{owner};
    }
  };
  guard_awaiter scoped_lock() noexcept { return {*this, permit_.acquire()}; }
private:
  semaphore permit_;
};
} // namespace faio::sync
#endif
