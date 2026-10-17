/**
 * @file faio/detail/sync/condition_variable.hpp
 * @brief 协程同步原语：异步条件变量。
 * @details 同步等待保存帧内节点，队列只借用地址。登记、通知与取消争夺唯一恢复权；跨线程通知经保存的调度器恢复，用户协程不在工作线程上阻塞等待。
 */
#ifndef FAIO_DETAIL_SYNC_CONDITION_VARIABLE_HPP
#define FAIO_DETAIL_SYNC_CONDITION_VARIABLE_HPP

#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/sync/mutex.hpp"
#include <concepts>
#include <mutex>

namespace faio::sync {
/// @brief wait 调用前必须持有用户 mutex。入队和解锁处于同一个内部临界区，
/// 因此通知者在修改条件后通知，不会发生“检查条件与挂起之间丢通知”。
class condition_variable {
 public:
  condition_variable() = default;

  condition_variable(const condition_variable&) = delete;

  condition_variable& operator=(const condition_variable&) = delete;

  struct wait_awaiter {
    condition_variable& cv;  ///< 借用当前等待关联的异步条件变量。
    mutex& user_mutex;  ///< 条件等待释放并在恢复前重新取得的用户异步互斥锁。
    detail::wait_node node;  ///< 嵌入对象的稳定等待或分派节点，入队后直到完成交接前不得移动。

    /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
    bool await_ready() const noexcept { return false; }

    /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
    bool await_suspend(std::coroutine_handle<> h) {
      {
        /// @brief 以条件变量内部锁串行化等待节点登记与通知摘链，用户互斥锁仍在本临界区内保持持有。
        std::lock_guard lock(cv.mutex_);
        /// @brief 先记录续体与登记线程调度器，通知或取消到来时才能回到正确运行时。
        node.capture(h);
        /// @brief 先把当前帧节点登记为可被通知的等待者，再释放用户互斥锁，堵住登记与解锁之间的丢失唤醒窗口。
        cv.waiters_.push(&node);
        /// @brief 等待已经登记后归还用户锁；通知者取得用户锁修改条件后不会错过该等待节点。
        user_mutex.unlock();
      }
      /// @brief 离开内部锁后订阅停止，令牌已停止时允许同步回调摘除节点而不会递归加锁。
      node.register_stop(cv.mutex_, cv.waiters_);
      /// @brief 用原子状态交接挂起资格；通知或取消已先到达时由当前执行流直接继续。
      return node.arm();
    }

    /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
    bool await_resume() const noexcept { return node.cancelled(); }
  };

  /// @brief 创建当前完成条件的协程等待器，具体取消语义由所属原语决定。
  template <class Predicate>
    requires std::predicate<Predicate&>
  task<void> wait(mutex& m, Predicate predicate) {
    /// @brief 始终在持有用户锁时复查条件；一次通知只提供重新检查机会，不保证谓词已经成立。
    while (!predicate()) {
      /// @brief 登记等待并释放用户锁，恢复时只返回取消原因，尚未恢复用户持锁不变量。
      const bool cancelled = co_await wait_awaiter{*this, m, {}};
      /// @brief wait 已释放用户锁，返回或抛取消以前必须重新取得锁。
      /// 同一 stop_token 已停止时普通 lock 可能再次取消，导致 guard 错误 unlock。
      co_await m.lock_uncancellable();
      /// @brief 用户锁已经不可取消地重新取得，此时才允许向调用方传播取消异常。
      if (cancelled)
        /// @brief 在等待协程上报告协作取消，节点已安全离队而不会再被通知方借用。
        throw operation_cancelled{};
    }
  }

  /// @brief 摘除一个等待者并在锁外发布通知，恢复责任只交接一次。
  void notify_one() {
    /// @brief 暂存锁内独占摘下的等待节点，释放队列锁后才向它发布恢复通知。
    detail::wait_node* node;
    {
      /// @brief 串行保护当前同步原语的等待队列及受锁管理的计数转换。
      std::lock_guard lock(mutex_);
      /// @brief 从等待队列摘除最早节点，当前通知者独占其恢复责任。
      node = waiters_.pop();
    }
    /// @brief 只有成功取得节点摘除权的路径才可在锁外通知该节点。
    if (node)
      /// @brief 锁外发布通知；节点可能随恢复立即销毁，通知之后不能继续访问它。
      node->wake();
  }

  /// @brief 在锁内摘除等待链，再在锁外逐个发布恢复通知。
  void notify_all() {
    /// @brief 先在锁内取得整条等待链，再在锁外发布恢复，避免用户代码争用内部队列锁。
    detail::wait_node* nodes;
    {
      /// @brief 串行保护当前同步原语的等待队列及受锁管理的计数转换。
      std::lock_guard lock(mutex_);
      /// @brief 在内部锁内一次摘除全部等待节点，当前通知方独占后续批量恢复责任。
      nodes = waiters_.take_all();
    }
    /// @brief 在队列锁外批量恢复独占摘下的等待链，唤醒前逐个保存后继。
    detail::wake_all(nodes);
  }

 private:
  std::mutex mutex_;  ///< 串行保护所属对象的共享状态；调度与用户回调应遵循该对象的锁外调用协议。
  detail::wait_queue waiters_;  ///< 不拥有节点的侵入式等待队列，节点由各等待者的协程帧保存。
};
}
#endif
