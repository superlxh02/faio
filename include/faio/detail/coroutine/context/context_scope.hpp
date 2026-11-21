/**
 * @file faio/detail/coroutine/context/context_scope.hpp
 * @brief 保存和恢复当前任务上下文的线程作用域守卫。
 */
#pragma once
#include "faio/detail/coroutine/context/current_context.hpp"

namespace faio::detail {
/** @brief 任务上下文的作用域绑定。 */
class scoped_task_context {
 public:
  /** @brief 保存原上下文，并安装本作用域的任务上下文。 */
  explicit scoped_task_context(const task_context& context) noexcept
      : previous_(capture_task_context()) {
    restore_task_context(context);
  }

  /** @brief 禁止复制上下文守卫。 */
  scoped_task_context(const scoped_task_context&) = delete;
  /** @brief 禁止复制赋值。 */
  scoped_task_context& operator=(const scoped_task_context&) = delete;

  /** @brief 退出作用域时恢复原任务上下文。 */
  ~scoped_task_context() {
    if (auto* context = try_current_thread_context())
      context->task_ctx = previous_;
  }

 private:
  task_context previous_; /**< 进入当前作用域前的任务上下文，析构时恢复。 */
};
}  // namespace faio::detail
