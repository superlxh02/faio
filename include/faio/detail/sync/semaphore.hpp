/**
 * @file faio/detail/sync/semaphore.hpp
 * @brief 协程同步原语：计数信号量与许可守卫。
 * @details 同步等待保存帧内节点，队列只借用地址。登记、通知与取消争夺唯一恢复权；跨线程通知经保存的调度器恢复，用户协程不在工作线程上阻塞等待。
 */
#ifndef FAIO_DETAIL_SYNC_SEMAPHORE_HPP
#define FAIO_DETAIL_SYNC_SEMAPHORE_HPP

#include "faio/detail/coroutine/coroutine_wait.hpp"
#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace faio::sync {
/// @brief 无竞争时只做一次 CAS；有竞争时在短临界区内把 permit 直接交给最早等待者。
class semaphore {
 public:
  explicit semaphore(std::ptrdiff_t permits = 0) : permits_(permits) {
    /// @brief 初始可用许可数必须非负，负值只保留给内部等待队列模式。
    if (permits < 0)
      throw std::invalid_argument("semaphore 初始令牌不能为负");
  }

  semaphore(const semaphore&) = delete;

  semaphore& operator=(const semaphore&) = delete;

  /// @brief 尝试立即取得许可，已有等待者时遵循原语的公平性约定。
  bool try_acquire() noexcept {
    /// @brief 先读可用许可数，只有正数路径才尝试以 CAS 消费一枚。
    auto count = permits_.load(std::memory_order_relaxed);
    /// @brief 正数表示存在尚未保留给队列等待者的可用许可。
    while (count > 0) {
      /// @brief 通过许可原子量竞争增减；竞争失败更新期望值后重试，成功才完成该次许可交接。
      if (permits_.compare_exchange_weak(
              count, count - 1, std::memory_order_acquire, std::memory_order_relaxed))
        return true;
    }
    return false;
  }

  struct acquire_awaiter {
    semaphore& sem;  ///< 借用等待操作所属的信号量，需存活到等待结束。
    detail::wait_node node;  ///< 保存在 awaiter/协程帧内的 FIFO 登记及取消节点。
    bool observe_stop{true};  ///< 是否订阅取消；条件变量清理时须不可取消地重新取得用户锁。

    /// @brief 先尝试立即领取许可，成功时无需登记等待节点。
    bool await_ready() noexcept { return sem.try_acquire(); }

    /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
    bool await_suspend(std::coroutine_handle<> h) {
      {
        /// @brief 把零许可到等待队列模式的转换与 release 慢路径串行化。
        std::lock_guard lock(sem.mutex_);
        /// @brief 在队列锁内把零计数切换为 -1，阻止 release 的无锁快路径跳过等待者。
        /// release 若先发布了令牌，则重新领取并立即继续；不会漏掉交接。
        for (;;) {
          /// @brief 挂起登记前再次尝试领取已发布许可，避免快速检查与入队之间遗漏释放。
          if (sem.try_acquire())
            return false;
          /// @brief 仅从无可用许可的零状态切换到等待模式，若释放先到则重新竞争许可。
          std::ptrdiff_t empty = 0;
          /// @brief 尝试把零许可切为等待队列模式，与并发 release 在同一原子量上确定先后顺序。
          if (sem.permits_.compare_exchange_strong(
                  empty, -1, std::memory_order_acq_rel, std::memory_order_relaxed)
              || empty == -1)
            break;
        }
        /// @brief 先记录续体与登记线程调度器，通知或取消到来时才能回到正确运行时。
        node.capture(h);
        /// @brief 在许可计数已阻止无锁释放路径后登记帧内节点，确保后续 release 能交接给等待者。
        sem.waiters_.push(&node);
      }
      /// @brief 正常等待订阅停止，条件变量清理阶段的重新加锁则保持不可取消。
      if (observe_stop)
        /// @brief 离开队列锁后构造停止回调，令牌已停止时可能同步回调并摘除节点。
        node.register_stop(sem.mutex_, sem.waiters_);
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

  /// @brief 创建异步许可等待器，等待期间通过节点协议响应通知与取消。
  acquire_awaiter acquire() noexcept { return {*this, {}, true}; }

  /** @brief 清理阶段的不取消等待，调用者保证归还许可；用于 CV 恢复用户 mutex。
   */
  acquire_awaiter acquire_uncancellable() noexcept { return {*this, {}, false}; }

  /// @brief permit 的析构自动归还令牌，适合跨 co_await 的临界区。
  class permit {
   public:
    explicit permit(semaphore& owner) noexcept : owner_(&owner) {}

    ~permit() {
      /// @brief 只有仍持有归还责任的守卫在析构时执行释放，移动后的源守卫不重复归还。
      if (owner_)
        /// @brief 按 RAII 规则归还一枚许可，可能直接把许可交给最早等待者。
        owner_->release();
    }

    permit(const permit&) = delete;

    permit& operator=(const permit&) = delete;

    permit(permit&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}

   private:
    semaphore* owner_;  ///< 借用归还目标；移动后源 permit 不再归还。
  };

  struct permit_awaiter {
    semaphore& sem;  ///< 成功后绑定 RAII permit 的归还目标。
    acquire_awaiter inner;  ///< 复用同一登记、唤醒与取消协议。

    /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
    bool await_ready() noexcept { return inner.await_ready(); }

    /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
    bool await_suspend(std::coroutine_handle<> h) { return inner.await_suspend(h); }

    /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
    permit await_resume() {
      /// @brief 先让底层等待器检查失败或取消，成功后才能构造持有许可的守卫。
      inner.await_resume();
      /// @brief 等待已经真正取得许可，返回负责在析构时归还该许可的守卫。
      return permit{sem};
    }
  };

  /// @brief guard 本身在调用者帧内构造；无竞争时不再额外分配 task 协程帧。
  permit_awaiter acquire_permit() noexcept { return {*this, acquire()}; }

  /// @brief 归还指定数量的许可；有等待者时直接按 FIFO 交接，否则发布为可用计数。
  void release(std::ptrdiff_t count = 1) {
    /// @brief 归还数量不能为负，避免把释放操作解释为隐式获取许可。
    if (count < 0)
      throw std::invalid_argument("semaphore release 不能为负");
    /// @brief 逐枚发布或交接许可，使每个被摘除等待者对应一枚真实归还的许可。
    for (std::ptrdiff_t i = 0; i < count; ++i) {
      /// @brief 先读取无等待者快路径的可用计数，是否仍可走快路径由后续 CAS 确认。
      auto available = permits_.load(std::memory_order_relaxed);
      /// @brief 非负值保证等待链表为空；CAS 与登记方的 0→-1 竞争建立唯一线性化点。
      /// 没有等待者时 release 只做一次原子修改，不再每次获取队列互斥锁。
      bool published = false;
      /// @brief 非负计数表示当前没有等待链交接责任，可以直接发布一枚可用许可。
      while (available >= 0) {
        /// @brief 递增前检查有符号许可计数上限，避免溢出破坏等待模式哨兵。
        if (available == std::numeric_limits<std::ptrdiff_t>::max())
          throw std::overflow_error("semaphore 令牌计数溢出");
        /// @brief 通过许可原子量竞争增减；竞争失败更新期望值后重试，成功才完成该次许可交接。
        if (permits_.compare_exchange_weak(
                available, available + 1, std::memory_order_release, std::memory_order_relaxed)) {
          /// @brief 当前许可已经在线性化点发布，后续不再为同一许可摘除等待者。
          published = true;
          break;
        }
      }
      /// @brief 成功完成无锁许可发布后直接处理下一枚，不进入等待队列慢路径。
      if (published)
        continue;
      /// @brief 暂存锁内独占摘下的等待节点，释放队列锁后才向它发布恢复通知。
      detail::wait_node* node;
      {
        /// @brief 串行保护当前同步原语的等待队列及受锁管理的计数转换。
        std::lock_guard lock(mutex_);
        /// @brief 从同步原语等待队列摘除最早节点，许可直接交给该等待者。
        node = waiters_.pop();
        /// @brief 等待者可能已全部取消；没有可交接节点时必须把本次许可恢复为可用计数。
        if (!node) {
          /// @brief 所有等待者可能已取消。此时恢复非负计数并发布这一枚新令牌。
          /// 另一个 release 可能已恢复计数，因此不能无条件把计数写成 1。
          auto old = permits_.load(std::memory_order_relaxed);
          for (;;) {
            /// @brief 慢路径同样检查计数溢出，不能因等待者取消而绕过容量安全约束。
            if (old == std::numeric_limits<std::ptrdiff_t>::max())
              throw std::overflow_error("semaphore 令牌计数溢出");
            /// @brief 等待哨兵转换为一枚可用许可；若其他释放者已恢复计数，则在其基础上继续累加。
            const auto next = old == -1 ? 1 : old + 1;
            /// @brief 通过许可原子量竞争增减；竞争失败更新期望值后重试，成功才完成该次许可交接。
            if (permits_.compare_exchange_weak(
                    old, next, std::memory_order_release, std::memory_order_relaxed))
              break;
          }
        } else if (!waiters_.head) {
          /// @brief 本次令牌直接给摘除者；最后一位离队后允许未来 release 走快路径。
          permits_.store(0, std::memory_order_release);
        }
      }
      /// @brief 只有成功取得节点摘除权的路径才可在锁外通知该节点。
      if (node)
        /// @brief 锁外发布通知；节点可能随恢复立即销毁，通知之后不能继续访问它。
        node->wake();
    }
  }

  std::ptrdiff_t available_permits() const noexcept {
    /// @brief acquire 读取当前可用计数；内部负哨兵只表示有等待者，对外按零可用许可报告。
    const auto count = permits_.load(std::memory_order_acquire);
    return count < 0 ? 0 : count;
  }

 private:
  std::atomic<std::ptrdiff_t> permits_;  ///< 非负是可用令牌数，-1 表示进入队列交接模式。
  std::mutex mutex_;  ///< 只保护有争用的登记、取消和 FIFO 交接。
  detail::wait_queue waiters_;  ///< 借用等待协程帧中的节点；有节点时计数保持 -1。
};
}
#endif
