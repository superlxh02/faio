/**
 * @file faio/detail/runtime/multi_thread/scheduler/local_execution_budget.hpp
 * @brief 连续快速槽协作预算。
 */
#pragma once

#include "faio/detail/coroutine/context/current_context.hpp"
#include <cstdint>

namespace faio::runtime::detail {
/**
 *  @brief 所属工作线程连续执行私有快速槽任务时共用的协作预算。
 * @details 不保存任务/帧地址，不标记可窃取槽，不向远端共享此状态。
 *          FIFO、全局或窃取任务开始新的链；driver/等待边界显式结束旧链。
 *          所有父子 task 仍通过原 TLS 额度执行条件让出。
 */
class local_execution_budget {
 public:
  /**  @brief 实际恢复协程时的 TLS 作用域，同时领取和归还连续执行链的剩余预算。 */
  class execution_scope {
   public:
    /**  @param from_fast 是否由本 worker 的私有快速槽取得本次任务。 */
    execution_scope(local_execution_budget& owner, bool from_fast) noexcept
        : owner_(owner),
          poll_(from_fast && owner.active_ && owner.remaining_ != 0
                    ? owner.remaining_
                    : ::faio::detail::cooperative_budget_limit) {}

    /** @brief 禁止复制构造执行上下文作用域。 */
    execution_scope(const execution_scope&) = delete;

    /** @brief 禁止复制赋值。 */
    execution_scope& operator=(const execution_scope&) = delete;

    /** @brief 销毁执行上下文作用域。 */
    ~execution_scope() {
      owner_.remaining_ = ::faio::detail::current_thread_context().poll.remaining;
      owner_.active_ = true;
    }

   private:
    local_execution_budget& owner_; /**< 所属线程的连续执行预算；跨线程任务不借用它。 */
    ::faio::detail::cooperative_poll_scope
        poll_; /**< 通过 scope 在退出时恢复外层 TLS 预算，支持嵌套及异常退出。 */
  };

  /**  @brief 选取后仅在实际 resume 周围调用，不把未执行的选择计为消耗。 */
  execution_scope begin(bool from_fast) noexcept { return execution_scope{*this, from_fast}; }

  /**  @brief 预算耗尽时先尝试本地 FIFO，只有 FIFO 为空时才允许快速槽以完整预算继续执行。 */
  bool exhausted() const noexcept { return active_ && remaining_ == 0; }

  /**
   *  @brief I/O 驱动、等待或没有就绪任务时结束连续执行链；后续快速槽任务也从完整预算开始。
   */
  void interrupt() noexcept {
    active_ = false;
    remaining_ = ::faio::detail::cooperative_budget_limit;
  }

 private:
  std::uint16_t remaining_{
      ::faio::detail::cooperative_budget_limit}; /**< 每条连续执行链最多允许 64 次检查。 */
  bool active_{false}; /**< 所属线程上是否刚完成了可连续接续的真实恢复。 */
};
}  // namespace faio::runtime::detail
