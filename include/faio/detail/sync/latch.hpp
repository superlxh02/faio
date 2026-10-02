#ifndef FAIO_DETAIL_SYNC_LATCH_HPP
#define FAIO_DETAIL_SYNC_LATCH_HPP

#include "faio/detail/coroutine/coroutine_wait.hpp"
#include <cstddef>
#include <mutex>
#include <stdexcept>

namespace faio::sync {
// 单次倒计时；计数归零后 wait 永远立即完成。
class latch {
public:
  explicit latch(std::ptrdiff_t count) : remaining_(count) {
    if (count < 0) throw std::invalid_argument("latch 计数不能为负");
  }
  latch(const latch&) = delete;
  latch& operator=(const latch&) = delete;
  void count_down(std::ptrdiff_t n = 1) {
    detail::wait_node* nodes{};
    {
      std::lock_guard lock(mutex_);
      if (n < 0 || n > remaining_) throw std::invalid_argument("latch count_down 越界");
      remaining_ -= n;
      if (remaining_ == 0) {
        nodes = waiters_.take_all();
      }
    }
    detail::wake_all(nodes);
  }
  bool try_wait() const {
    std::lock_guard lock(mutex_);
    return remaining_ == 0;
  }
  struct awaiter {
    latch& self;
    detail::wait_node node;
    bool await_ready() const { return self.try_wait(); }
    bool await_suspend(std::coroutine_handle<> h) {
      {
        std::lock_guard lock(self.mutex_);
        if (self.remaining_ == 0) return false;
        node.capture(h);
        self.waiters_.push(&node);
      }
      node.register_stop(self.mutex_, self.waiters_);
      return node.arm();
    }
    void await_resume() const {
      if (node.cancelled()) throw operation_cancelled{};
    }
  };
  awaiter wait() noexcept { return {*this, {}}; }
private:
  mutable std::mutex mutex_;
  std::ptrdiff_t remaining_;
  detail::wait_queue waiters_;
};
} // namespace faio::sync
#endif
