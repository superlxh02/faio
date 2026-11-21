/**
 * @file faio/detail/sync/condition_variable.hpp
 * @brief 异步条件变量。
 */
#ifndef FAIO_DETAIL_SYNC_CONDITION_VARIABLE_HPP
#define FAIO_DETAIL_SYNC_CONDITION_VARIABLE_HPP

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/sync/mutex.hpp"
#include <concepts>
#include <mutex>

namespace faio::sync {
/**
 * @brief 调用 wait 前必须持有用户 mutex。等待者在同一个内部临界区内入队并解锁，
 * 因此通知方在修改条件后发出通知，不会在检查条件与挂起之间丢失通知。
 */
template <class S = default_scheduler>
class basic_condition_variable {
 public:
  /** @brief 构造协程条件变量。 */
  basic_condition_variable() = default;

  /** @brief 禁止复制构造协程条件变量。 */
  basic_condition_variable(const basic_condition_variable&) = delete;

  /** @brief 禁止复制赋值。 */
  basic_condition_variable& operator=(const basic_condition_variable&) = delete;

  /** @brief 条件变量等待器。 */
  struct wait_awaiter {
    basic_condition_variable& cv;    /**< 借用当前等待关联的异步条件变量。 */
    basic_mutex<S>& user_mutex;      /**< 条件等待释放并在恢复前重新取得的用户异步互斥锁。 */
    detail::basic_wait_node<S> node; /**< 内嵌的条件等待节点，从入队到完成交接期间不得移动。 */

    /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
    bool await_ready() const noexcept { return false; }

    /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
    template <class Promise>
      requires detail::matching_wait_scheduler<S, Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      {
        std::lock_guard lock(cv.mutex_);
        node.capture(h);
        cv.waiters_.push(&node);
        user_mutex.unlock();
      }
      node.register_stop(cv.mutex_, cv.waiters_);
      return node.try_suspend();
    }

    /** @brief 返回本次等待是否被取消。 */
    bool await_resume() const noexcept { return node.cancelled(); }
  };

  /** @brief 等待谓词成立，并遵循条件变量的取消约定。 */
  template <class Predicate>
    requires std::predicate<Predicate&>
  task<void, S> wait(basic_mutex<S>& m, Predicate predicate) {
    while (!predicate()) {
      const bool cancelled = co_await wait_awaiter{*this, m, {}};
      co_await m.lock_uncancellable();
      if (cancelled)
        throw operation_cancelled{};
    }
  }

  /** @brief 摘除一个等待者并在锁外发布通知，恢复责任只交接一次。 */
  void notify_one() {
    detail::basic_wait_node<S>* node;
    {
      std::lock_guard lock(mutex_);
      node = waiters_.pop();
    }
    if (node)
      node->wake();
  }

  /** @brief 在锁内摘除等待链，再在锁外逐个发布恢复通知。 */
  void notify_all() {
    detail::basic_wait_node<S>* nodes;
    {
      std::lock_guard lock(mutex_);
      nodes = waiters_.take_all();
    }
    detail::wake_all(nodes);
  }

 private:
  std::mutex mutex_; /**< 串行保护条件变量状态；调度和用户回调须在锁外执行。 */
  detail::basic_wait_queue<S>
      waiters_; /**< 不拥有节点的侵入式等待队列，节点由各等待者的协程帧保存。 */
};

using condition_variable = basic_condition_variable<>;
}  // namespace faio::sync
#endif
