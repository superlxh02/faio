/**
 * @file current_context.hpp
 * @brief 访问和安装当前任务上下文及协作执行预算。
 */
#ifndef FAIO_DETAIL_COROUTINE_CURRENT_CONTEXT_HPP
#define FAIO_DETAIL_COROUTINE_CURRENT_CONTEXT_HPP

#include "faio/detail/coroutine/context/thread_context.hpp"

namespace faio::detail {
/** @brief 取得线程中保存的当前任务上下文的只读引用；任务挂起期间以 promise 中保存的状态为准。 */
inline const task_context& current_task_context() noexcept {
  return current_thread_context().task_ctx;
}

/** @brief 捕获当前任务上下文。 */
inline task_context capture_task_context() noexcept {
  return current_task_context();
}

/** @brief 安装任务属性，保留工作线程的服务绑定和当前执行链的剩余预算。 */
inline void install_task_context(const task_context& context) noexcept {
  current_thread_context().task_ctx = context;
}

/** @brief 原生 task 直接进入或返回时恢复上下文；属性值和所有权关系均相同时沿用当前上下文。 */
inline task_context& restore_native_task_context(const task_context& context) noexcept {
  auto& active = current_thread_context().task_ctx;
  const auto& owner = context.cancellation_owner;
  const auto& current_owner = active.cancellation_owner;
  if (active.scope != context.scope || active.scheduling != context.scheduling
      || active.cancellation_policy != context.cancellation_policy
      || active.stop_token != context.stop_token || current_owner.get() != owner.get()
      || current_owner.owner_before(owner) || owner.owner_before(current_owner)
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
      || active.external_scope.host != context.external_scope.host
      || active.external_scope.root_state != context.external_scope.root_state
#endif
      ) [[unlikely]] {
    active = context;
  }
  return active;
}

/** @brief 安装完整任务上下文以恢复状态。 */
inline void restore_task_context(const task_context& context) noexcept {
  install_task_context(context);
}

/**
 * @brief 为调度器实际恢复协程的过程设置预算作用域；父子任务直接转移时不重置预算。
 * @details 从 FIFO、全局队列或窃取路径恢复时，默认将预算补为 64；连续执行私有 fast 链时显式沿用剩余额度。
 *          退出时仅恢复外层预算，支持嵌套驱动和异常退出。
 */
class cooperative_poll_scope {
 public:
  /** @brief 构造协作轮询作用域。 */
  cooperative_poll_scope() noexcept : cooperative_poll_scope(cooperative_budget_limit) {}

  /** @brief 构造协作轮询作用域。 */
  explicit cooperative_poll_scope(std::uint16_t initial) noexcept
      : previous_(current_thread_context().poll.remaining) {
    current_thread_context().poll.remaining = initial;
  }

  /** @brief 禁止复制轮询作用域。 */
  cooperative_poll_scope(const cooperative_poll_scope&) = delete;
  /** @brief 禁止复制赋值。 */
  cooperative_poll_scope& operator=(const cooperative_poll_scope&) = delete;

  /** @brief 退出作用域时恢复外层协作预算。 */
  ~cooperative_poll_scope() {
    if (auto* context = try_current_thread_context())
      context->poll.remaining = previous_;
  }

 private:
  std::uint16_t previous_;
};
}  // namespace faio::detail
#endif
