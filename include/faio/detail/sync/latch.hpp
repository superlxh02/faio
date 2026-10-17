/**
 * @file faio/detail/sync/latch.hpp
 * @brief 协程同步原语：一次性协程倒计数。
 * @details 同步等待保存帧内节点，队列只借用地址。登记、通知与取消争夺唯一恢复权；跨线程通知经保存的调度器恢复，用户协程不在工作线程上阻塞等待。
 */
#ifndef FAIO_DETAIL_SYNC_LATCH_HPP
#define FAIO_DETAIL_SYNC_LATCH_HPP

#include "faio/detail/coroutine/coroutine_wait.hpp"
#include <cstddef>
#include <mutex>
#include <stdexcept>

namespace faio::sync {
/// @brief 单次倒计时；计数归零后 wait 永远立即完成。
class latch {
 public:
  explicit latch(std::ptrdiff_t count) : remaining_(count) {
    /// @brief 归还数量不能为负，避免把释放操作解释为隐式获取许可。
    if (count < 0)
      throw std::invalid_argument("latch 计数不能为负");
  }

  latch(const latch&) = delete;

  latch& operator=(const latch&) = delete;

  /// @brief 减少剩余计数，首次归零时向全部已登记等待者发布完成。
  void count_down(std::ptrdiff_t n = 1) {
    /// @brief 暂存当前路径独占摘除的完成链，空值表示尚无需要批量通知的等待者。
    detail::wait_node* nodes{};
    {
      /// @brief 串行保护当前同步原语的等待队列及受锁管理的计数转换。
      std::lock_guard lock(mutex_);
      /// @brief 拒绝负递减量或超过剩余计数的递减，防止倒计数下溢。
      if (n < 0 || n > remaining_)
        throw std::invalid_argument("latch count_down 越界");
      /// @brief 在锁内归还指定数量的未完成票数，只有首次归零能够取得等待链。
      remaining_ -= n;
      /// @brief 计数已经满足全部等待条件，后续等待者会直接观察完成。
      if (remaining_ == 0) {
        /// @brief 在内部锁内一次摘除全部等待节点，当前通知方独占后续批量恢复责任。
        nodes = waiters_.take_all();
      }
    }
    /// @brief 在队列锁外批量恢复独占摘下的等待链，唤醒前逐个保存后继。
    detail::wake_all(nodes);
  }

  /// @brief 无阻塞地检查一次性计数是否已经归零。
  bool try_wait() const {
    /// @brief 串行保护当前同步原语的等待队列及受锁管理的计数转换。
    std::lock_guard lock(mutex_);
    return remaining_ == 0;
  }

  struct awaiter {
    latch& self;  ///< 借用所属对象的稳定地址，回调或等待期间不得迁移或销毁。
    detail::wait_node node;  ///< 嵌入对象的稳定等待或分派节点，入队后直到完成交接前不得移动。

    /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
    bool await_ready() const { return self.try_wait(); }

    /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
    bool await_suspend(std::coroutine_handle<> h) {
      {
        /// @brief 把本轮计数、节点登记及整链摘除放入同一串行边界。
        std::lock_guard lock(self.mutex_);
        /// @brief 初次快检之后计数可能已归零，持内部锁再次确认后无需登记节点。
        if (self.remaining_ == 0)
          return false;
        /// @brief 先记录续体与登记线程调度器，通知或取消到来时才能回到正确运行时。
        node.capture(h);
        /// @brief 当前轮尚未满足，把已初始化的帧内节点登记到 FIFO 队尾。
        self.waiters_.push(&node);
      }
      /// @brief 释放内部锁后注册可取消等待，正常完成与取消仍按队列锁争夺摘除权。
      node.register_stop(self.mutex_, self.waiters_);
      /// @brief 用原子状态交接挂起资格；通知或取消已先到达时由当前执行流直接继续。
      return node.arm();
    }

    /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
    void await_resume() const {
      /// @brief 恢复后以节点唯一终态判断是否取消，通知先胜出时不把迟到停止解释为取消。
      if (node.cancelled())
        /// @brief 在等待协程上报告协作取消，节点已安全离队而不会再被通知方借用。
        throw operation_cancelled{};
    }
  };

  /// @brief 创建当前完成条件的协程等待器，具体取消语义由所属原语决定。
  awaiter wait() noexcept { return {*this, {}}; }

 private:
  mutable std::mutex mutex_;  ///< 串行保护所属对象的共享状态；调度与用户回调应遵循该对象的锁外调用协议。
  std::ptrdiff_t remaining_;  ///< 尚未归还的完成票数，归零后永久保持等待条件满足。
  detail::wait_queue waiters_;  ///< 不拥有节点的侵入式等待队列，节点由各等待者的协程帧保存。
};
}
#endif
