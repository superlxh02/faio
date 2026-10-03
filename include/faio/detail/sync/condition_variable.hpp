#ifndef FAIO_DETAIL_SYNC_CONDITION_VARIABLE_HPP
#define FAIO_DETAIL_SYNC_CONDITION_VARIABLE_HPP

#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/sync/mutex.hpp"
#include <concepts>
#include <mutex>

namespace faio::sync {
// wait 调用前必须持有用户 mutex。入队和解锁处于同一个内部临界区，
// 因此通知者在修改条件后通知，不会发生“检查条件与挂起之间丢通知”。
class condition_variable {
public:
  condition_variable() = default;
  condition_variable(const condition_variable &) = delete;
  condition_variable &operator=(const condition_variable &) = delete;

  struct wait_awaiter {
    condition_variable &cv;
    mutex &user_mutex;
    detail::wait_node node;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
      {
        std::lock_guard lock(cv.mutex_);
        node.capture(h);
        cv.waiters_.push(&node);
        user_mutex.unlock();
      }
      node.register_stop(cv.mutex_, cv.waiters_);
      return node.arm();
    }
    bool await_resume() const noexcept { return node.cancelled(); }
  };

  template <class Predicate>
    requires std::predicate<Predicate &>
  task<void> wait(mutex &m, Predicate predicate) {
    while (!predicate()) {
      const bool cancelled = co_await wait_awaiter{*this, m, {}};
      // wait 已释放用户锁，返回或抛取消以前必须重新取得锁。
      // 同一 stop_token 已停止时普通 lock 可能再次取消，导致 guard 错误
      // unlock。
      co_await m.lock_uncancellable();
      if (cancelled)
        throw operation_cancelled{};
    }
  }
  void notify_one() {
    detail::wait_node *node;
    {
      std::lock_guard lock(mutex_);
      node = waiters_.pop();
    }
    if (node)
      node->wake();
  }
  void notify_all() {
    detail::wait_node *nodes;
    {
      std::lock_guard lock(mutex_);
      nodes = waiters_.take_all();
    }
    detail::wake_all(nodes);
  }

private:
  std::mutex mutex_;
  detail::wait_queue waiters_;
};
} // namespace faio::sync
#endif
