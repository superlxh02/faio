/**
 * @file faio/detail/runtime/multi_thread/scheduler/local_execution_budget.hpp
 * @brief 运行时调度、线程协调与定时驱动：连续快速槽协作预算。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#pragma once

#include "faio/detail/coroutine/task_context.hpp"
#include <cstdint>

namespace faio::runtime::detail {
/** @brief 所属 worker 连续私有快速恢复链共用的协作额度。
 * @details 不保存任务/帧地址，不标记可窃取槽，不向远端共享此状态。
 *          FIFO、全局或窃取任务开始新的链；driver/等待边界显式结束旧链。
 *          所有父子 task 仍通过原 TLS 额度执行条件让出。
 */
class local_execution_budget {
 public:
  /** @brief 一次真正 resume 的 TLS 作用域，同时领取/归还连续链剩余额度。 */
  class execution_scope {
   public:
    /** @param from_fast 是否由本 worker 的私有快速槽取得本次任务。 */
    execution_scope(local_execution_budget& owner, bool from_fast) noexcept
        /// @brief 仅借用栈上的调度器；作用域不会逃出 execute。
        : owner_(owner),
          poll_(from_fast && owner.active_ && owner.remaining_ != 0
                    /// @brief 连续 fast 链只能使用此前尚未消耗的额度。
                    ? owner.remaining_
                    /// @brief 新链/仅 fast 耗尽仍可推进。
                    : ::faio::detail::cooperative_budget_limit) {}

    execution_scope(const execution_scope&) = delete;

    execution_scope& operator=(const execution_scope&) = delete;

    ~execution_scope() {
      /// @brief 先取得本次真实消耗结果。
      owner_.remaining_ = ::faio::detail::current_cooperative_budget;
      /// @brief 随后 poll_ 析构还原外层 TLS，不能把外层值当成本链结果。
      owner_.active_ = true;
    }

   private:
    local_execution_budget& owner_;  ///< 当前所属调度器；跨线程任务不借用它。
    ::faio::detail::cooperative_poll_scope poll_;  ///< 与原 scope 相同的嵌套/异常 TLS 还原协议。
  };

  /** @brief 选取后仅在实际 resume 周围调用，不把未执行的选择计为消耗。 */
  execution_scope begin(bool from_fast) noexcept { return execution_scope{*this, from_fast}; }

  /** @brief 预算已耗尽时先给本地 FIFO 一次机会，空 FIFO 才允许 fresh fast。 */
  bool exhausted() const noexcept { return active_ && remaining_ == 0; }

  /** @brief IO 驱动、等待或无就绪任务结束连续执行链；之后 fast
   * 也从完整额度开始。 */
  void interrupt() noexcept {
    /// @brief 不保留跨真正异步等待的“每64包强制再让出”状态。
    active_ = false;
    /// @brief 私有诊断状态同样恢复初值。
    remaining_ = ::faio::detail::cooperative_budget_limit;
  }

 private:
  std::uint16_t remaining_{::faio::detail::cooperative_budget_limit};  ///< 每条连续链至多64次检查。
  bool active_{false};  ///< 所属线程上是否刚完成了可连续接续的真实恢复。
};
}
