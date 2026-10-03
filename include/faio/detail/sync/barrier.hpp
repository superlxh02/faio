#ifndef FAIO_DETAIL_SYNC_BARRIER_HPP
#define FAIO_DETAIL_SYNC_BARRIER_HPP

#include "faio/detail/coroutine/coroutine_wait.hpp"
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace faio::sync {
// 可重复使用的 N 方屏障。最后到达者推进代数并批量唤醒上一代等待者。
class barrier {
public:
  explicit barrier(std::ptrdiff_t participants)
      : participants_(participants), remaining_(participants) {
    if (participants <= 0)
      throw std::invalid_argument("barrier 参与者必须大于零");
  }
  barrier(const barrier &) = delete;
  barrier &operator=(const barrier &) = delete;
  struct awaiter {
    barrier &self;
    detail::wait_node node;
    struct cancel_callback {
      barrier *self;
      detail::wait_node *node;
      void operator()() const noexcept {
        detail::wait_node *others{};
        {
          std::lock_guard lock(self->mutex_);
          if (!node->queued)
            return; // 完成者已接管节点，正常通知会负责恢复。
          self->waiters_.remove(node);
          self->broken_ = true;
          others = self->waiters_.take_all();
        }
        node->cancel();
        detail::cancel_all(others);
      }
    };
    std::optional<std::stop_callback<cancel_callback>> callback;
    explicit awaiter(barrier &value) : self(value) {}
    awaiter(awaiter &&other) noexcept
        : self(other.self), node(std::move(other.node)) {}
    awaiter(const awaiter &) = delete;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
      detail::wait_node *nodes{};
      bool registered = false;
      {
        std::lock_guard lock(self.mutex_);
        if (self.broken_)
          throw operation_cancelled{};
        if (--self.remaining_ == 0) {
          self.remaining_ = self.participants_;
          nodes = self.waiters_.take_all();
        } else {
          node.capture(h);
          self.waiters_.push(&node);
          registered = true;
        }
      }
      // 另一 worker 即使已经完成本代通知，arm 也会看到状态变化并返回 false。
      if (registered) {
        auto token = ::faio::detail::current_stop_token;
        if (token.stop_possible())
          callback.emplace(token, cancel_callback{&self, &node});
        return node.arm();
      }
      detail::wake_all(nodes);
      return false;
    }
    void await_resume() const {
      if (node.cancelled())
        throw operation_cancelled{};
    }
  };
  awaiter arrive_and_wait() noexcept { return awaiter{*this}; }

private:
  std::mutex mutex_;
  const std::ptrdiff_t participants_;
  std::ptrdiff_t remaining_;
  bool broken_{};
  detail::wait_queue waiters_;
};
} // namespace faio::sync
#endif
