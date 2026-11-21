/**
 * @file faio/detail/sync/barrier.hpp
 * @brief 可取消协程屏障。
 */
#ifndef FAIO_DETAIL_SYNC_BARRIER_HPP
#define FAIO_DETAIL_SYNC_BARRIER_HPP

#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/wait/queue.hpp"
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace faio::sync {
/** @brief 可重复使用的 N 方屏障。最后到达的参与者切换到下一轮，并批量唤醒本轮等待者。 */
template <class S = default_scheduler>
class basic_barrier {
 public:
  /** @brief 构造协程屏障。 */
  explicit basic_barrier(std::ptrdiff_t participants)
      : participants_(participants), remaining_(participants) {
    if (participants <= 0)
      throw std::invalid_argument("barrier 参与者必须大于零");
  }

  /** @brief 禁止复制构造协程屏障。 */
  basic_barrier(const basic_barrier&) = delete;

  /** @brief 禁止复制赋值。 */
  basic_barrier& operator=(const basic_barrier&) = delete;

  /** @brief 屏障等待器。 */
  struct awaiter {
    basic_barrier& self;             /**< 借用所属屏障；屏障在回调或等待期间不得移动或销毁。 */
    detail::basic_wait_node<S> node; /**< 内嵌的屏障等待节点，从入队到完成交接期间不得移动。 */

    /** @brief 协作取消回调。 */
    struct cancel_callback {
      basic_barrier* self; /**< 借用所属屏障；屏障在回调或等待期间不得移动或销毁。 */
      detail::basic_wait_node<S>*
          node; /**< 借用正在等待的帧内节点，回调注销与完成交接前必须保持有效。 */

      void operator()() const noexcept {
        detail::basic_wait_node<S>* others{};
        {
          std::lock_guard lock(self->mutex_);
          if (!node->queued)
            return;
          self->waiters_.remove(node);
          self->broken_ = true;
          others = self->waiters_.take_all();
        }
        node->cancel();
        detail::cancel_all(others);
      }
    };

    std::optional<std::stop_callback<cancel_callback>>
        callback; /**< 按需就地保存的停止回调，析构时注销并等待在途回调结束。 */

    /** @brief 构造屏障等待器。 */
    explicit awaiter(basic_barrier& value) : self(value) {}

    /** @brief 移动屏障等待器。 */
    awaiter(awaiter&& other) noexcept : self(other.self), node(std::move(other.node)) {}

    /** @brief 禁止复制屏障等待器。 */
    awaiter(const awaiter&) = delete;

    /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
    bool await_ready() const noexcept { return false; }

    /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
    template <class Promise>
      requires detail::matching_wait_scheduler<S, Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      node.capture(h);
      detail::basic_wait_node<S>* nodes{};
      bool registered = false;
      {
        std::lock_guard lock(self.mutex_);
        if (self.broken_)
          throw operation_cancelled{};
        if (--self.remaining_ == 0) {
          self.remaining_ = self.participants_;
          nodes = self.waiters_.take_all();
        } else {
          self.waiters_.push(&node);
          registered = true;
        }
      }
      if (registered) {
        auto token = ::faio::detail::current_task_context().stop_token;
        if (token.stop_possible())
          callback.emplace(token, cancel_callback{&self, &node});
        return node.try_suspend();
      }
      detail::wake_all(nodes);
      return false;
    }

    /** @brief 等待结束后检查取消状态。 */
    void await_resume() const {
      if (node.cancelled())
        throw operation_cancelled{};
    }
  };

  /** @brief 登记本轮到达并等待全部参与者，取消时按屏障破坏协议处理。 */
  awaiter arrive_and_wait() noexcept { return awaiter{*this}; }

 private:
  std::mutex mutex_;                  /**< 串行保护屏障状态；调度和用户回调须在锁外执行。 */
  const std::ptrdiff_t participants_; /**< 屏障每一轮必须到达的固定参与者数量。 */
  std::ptrdiff_t remaining_;          /**< 当前轮尚未到达的参与人数，由屏障内部锁保护。 */
  bool broken_{};                     /**< 屏障是否因取消等原因被打破，等待者据此报告失败。 */
  detail::basic_wait_queue<S>
      waiters_; /**< 不拥有节点的侵入式等待队列，节点由各等待者的协程帧保存。 */
};

using barrier = basic_barrier<>;
}  // namespace faio::sync
#endif
