/**
 * @file faio/detail/sync/barrier.hpp
 * @brief 协程同步原语：可取消协程屏障。
 * @details 同步等待保存帧内节点，队列只借用地址。登记、通知与取消争夺唯一恢复权；跨线程通知经保存的调度器恢复，用户协程不在工作线程上阻塞等待。
 */
#ifndef FAIO_DETAIL_SYNC_BARRIER_HPP
#define FAIO_DETAIL_SYNC_BARRIER_HPP

#include "faio/detail/coroutine/coroutine_wait.hpp"
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace faio::sync {
/// @brief 可重复使用的 N 方屏障。最后到达者推进代数并批量唤醒上一代等待者。
class barrier {
 public:
  explicit barrier(std::ptrdiff_t participants)
      : participants_(participants), remaining_(participants) {
    /// @brief 重复屏障必须至少有一个参与者，否则无法建立合法的一轮到达条件。
    if (participants <= 0)
      throw std::invalid_argument("barrier 参与者必须大于零");
  }

  barrier(const barrier&) = delete;

  barrier& operator=(const barrier&) = delete;

  struct awaiter {
    barrier& self;  ///< 借用所属对象的稳定地址，回调或等待期间不得迁移或销毁。
    detail::wait_node node;  ///< 嵌入对象的稳定等待或分派节点，入队后直到完成交接前不得移动。

    struct cancel_callback {
      barrier* self;  ///< 借用所属对象的稳定地址，回调或等待期间不得迁移或销毁。
      detail::wait_node* node;  ///< 借用正在等待的帧内节点，回调注销与完成交接前必须保持有效。

      void operator()() const noexcept {
        /// @brief 暂存因屏障破坏需要一并取消的其他节点链，链表将在锁外处理。
        detail::wait_node* others{};
        {
          /// @brief 停止回调与屏障最后到达者共用内部锁，确保每个节点只被一方摘除。
          std::lock_guard lock(self->mutex_);
          /// @brief 完成者已接管节点，正常通知会负责恢复。
          if (!node->queued)
            return;
          /// @brief 取消回调取得仍在队列中的当前节点，阻止正常屏障完成再次通知它。
          self->waiters_.remove(node);
          /// @brief 一名参与者取消会打破屏障，后续到达者不能继续等待无法满足的原参与人数。
          self->broken_ = true;
          /// @brief 把当前轮其余等待者一起摘除，锁外统一发布取消，避免其永久挂起。
          others = self->waiters_.take_all();
        }
        /// @brief 先发布当前节点的取消终态；调用后该节点可能随恢复立即销毁。
        node->cancel();
        /// @brief 取消其余已摘除节点，遍历时在恢复每个节点前先保存后继。
        detail::cancel_all(others);
      }
    };

    std::optional<std::stop_callback<cancel_callback>> callback;  ///< 按需就地保存的停止回调，析构时注销并等待在途回调结束。

    explicit awaiter(barrier& value) : self(value) {}

    awaiter(awaiter&& other) noexcept : self(other.self), node(std::move(other.node)) {}

    awaiter(const awaiter&) = delete;

    /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
    bool await_ready() const noexcept { return false; }

    /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
    bool await_suspend(std::coroutine_handle<> h) {
      /// @brief 暂存当前路径独占摘除的完成链，空值表示尚无需要批量通知的等待者。
      detail::wait_node* nodes{};
      /// @brief 区分当前到达者是否需要等待；最后到达者负责唤醒整轮而不把自己入队。
      bool registered = false;
      {
        /// @brief 把本轮计数、节点登记及整链摘除放入同一串行边界。
        std::lock_guard lock(self.mutex_);
        /// @brief 已打破的屏障拒绝新一轮等待，直接在调用方协程报告取消。
        if (self.broken_)
          /// @brief 在等待协程上报告协作取消，节点已安全离队而不会再被通知方借用。
          throw operation_cancelled{};
        /// @brief 本次到达耗尽当前轮剩余人数时，当前线程取得整轮完成发布责任。
        if (--self.remaining_ == 0) {
          /// @brief 为下一轮恢复固定参与人数，旧一轮节点将在本临界区内全部摘走。
          self.remaining_ = self.participants_;
          /// @brief 摘除刚完成这一轮的全部等待者，后续到达者进入新的等待链。
          nodes = self.waiters_.take_all();
        } else {
          /// @brief 先记录续体与登记线程调度器，通知或取消到来时才能回到正确运行时。
          node.capture(h);
          /// @brief 当前轮尚未满足，把已初始化的帧内节点登记到 FIFO 队尾。
          self.waiters_.push(&node);
          /// @brief 标记当前节点已经入队，释放内部锁后才构造停止回调并交接挂起资格。
          registered = true;
        }
      }
      /// @brief 另一 worker 即使已经完成本代通知，arm 也会看到状态变化并返回 false。
      if (registered) {
        /// @brief 捕获当前任务停止令牌，跨线程回调仍作用于本次已登记节点。
        auto token = ::faio::detail::current_stop_token;
        /// @brief 没有停止源时跳过回调存储与登记，不改变正常屏障完成协议。
        if (token.stop_possible())
          /// @brief 锁外登记回调；若停止已发生，构造可同步打破屏障并使后续 arm 失败。
          callback.emplace(token, cancel_callback{&self, &node});
        /// @brief 用原子状态交接挂起资格；通知或取消已先到达时由当前执行流直接继续。
        return node.arm();
      }
      /// @brief 在队列锁外批量恢复独占摘下的等待链，唤醒前逐个保存后继。
      detail::wake_all(nodes);
      return false;
    }

    /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
    void await_resume() const {
      /// @brief 恢复后以节点唯一终态判断是否取消，通知先胜出时不把迟到停止解释为取消。
      if (node.cancelled())
        /// @brief 在等待协程上报告协作取消，节点已安全离队而不会再被通知方借用。
        throw operation_cancelled{};
    }
  };

  /// @brief 登记本轮到达并等待全部参与者，取消时按屏障破坏协议处理。
  awaiter arrive_and_wait() noexcept { return awaiter{*this}; }

 private:
  std::mutex mutex_;  ///< 串行保护所属对象的共享状态；调度与用户回调应遵循该对象的锁外调用协议。
  const std::ptrdiff_t participants_;  ///< 屏障每一轮必须到达的固定参与者数量。
  std::ptrdiff_t remaining_;  ///< 当前轮尚未到达的参与人数，由屏障内部锁保护。
  bool broken_{};  ///< 屏障是否因取消等原因被打破，等待者据此报告失败。
  detail::wait_queue waiters_;  ///< 不拥有节点的侵入式等待队列，节点由各等待者的协程帧保存。
};
}
#endif
