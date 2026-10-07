#ifndef FAIO_DETAIL_COROUTINE_TASK_CONTEXT_HPP
#define FAIO_DETAIL_COROUTINE_TASK_CONTEXT_HPP

#include "faio/detail/coroutine/execution_thread.hpp"
#include "faio/detail/coroutine/task_tracker.hpp"
#include "faio/detail/runtime/common/external_work.hpp"
#include <cstdint>
#include <stop_token>
#include <memory>
#include <optional>
#include <utility>

namespace faio {
// 当前为上下文元数据；具体调度队列尚未按优先级分层。
// 优先级元数据：low 低、normal 默认、high 高；当前队列仍未按它分层调度。
enum class task_priority : unsigned char { low, normal, high };

namespace detail {
// 当前协程的停止令牌。worker 每次执行根协程时清空，task 的 awaiter
// 恢复用户代码前重建；挂起后的任务上下文以 promise 中的副本为准。
// 当前用户协程的协作停止观察端，普通线程或无停止源的任务默认为空。
inline thread_local std::stop_token current_stop_token{};
enum class cancellation_error_policy { fatal_if_unobserved, normal_if_stop_requested };
// This owner holds cancellation wiring only, never a coroutine frame or result.
// Keeping an ancestor callback alive therefore cannot cycle through child values.
struct cancellation_state {
  struct forward {
    std::stop_source* target;
    void operator()() const noexcept { target->request_stop(); }
  };
  std::shared_ptr<cancellation_state> parent;
  std::stop_source source;
  std::optional<std::stop_callback<forward>> link;
  cancellation_state(std::stop_token token, std::shared_ptr<cancellation_state> owner)
      : parent(std::move(owner)) {
    if (token.stop_possible()) link.emplace(token, forward{&source});
  }
};
inline thread_local cancellation_error_policy current_cancellation_policy{
    cancellation_error_policy::fatal_if_unobserved};
inline thread_local external_scope_ref current_external_scope{};
inline thread_local std::shared_ptr<cancellation_state> current_cancellation_owner{};

/// @brief 调度执行作用域共用的协作预算；父子对称转移和连续私有 fast 链不重置。
inline constexpr std::uint16_t cooperative_budget_limit = 64;
inline thread_local std::uint16_t current_cooperative_budget = cooperative_budget_limit;

/** @brief 调度器 resume 的 TLS 预算作用域，退出后还原外层剩余额度。
 * @details 只有真正调度恢复才建立此作用域；直接 co_await 子任务继续消耗同一
 * TLS。 默认构造补64；multi_thread 的连续私有 fast 链显式传入尚余值。
 *          current_thread 与自定义调度器的默认每次恢复补64语义保持。
 *          保存/还原支持外部嵌套驱动与异常退出，不让内层任务改变外层剩余预算。
 */
class cooperative_poll_scope {
 public:
  cooperative_poll_scope() noexcept : cooperative_poll_scope(cooperative_budget_limit) {}

  /** @brief 调度器显式选择本轮额度；仅连续私有 fast 链传入已保存的剩余值。 */
  explicit cooperative_poll_scope(std::uint16_t initial) noexcept
      : previous_(current_cooperative_budget) {
    current_cooperative_budget = initial;  // FIFO/全局/窃取/IO边界仍选择完整64。
  }

  cooperative_poll_scope(const cooperative_poll_scope&) = delete;

  cooperative_poll_scope& operator=(const cooperative_poll_scope&) = delete;

  ~cooperative_poll_scope() { current_cooperative_budget = previous_; }

 private:
  std::uint16_t previous_;  ///< 借用线程的外层执行额度，跨任务帧不添加 epoch 或共享状态。
};

// 父任务在 await_suspend 传递调度器、停止请求和作用域。worker_id 是动态
// 查询，因为可移动任务每次恢复可能落在不同的 worker。
// 保存于用户 task promise 的执行属性集合。
// 父子直接 co_await 时按值继承；其中的调度器与 tracker 指针只借用外部对象。
struct task_context {
  // 当前任务所属的根任务计数组，恢复 TLS 后供派生 spawn 登记。
  ::faio::detail::task_tracker* scope{};  // 借用任务组，供派生根任务登记。
  // 所属运行时的轻量调度接口，让出与等待唤醒使用它。
  scheduler_ref scheduler{};  // 借用目标调度器，不延长运行时生命周期。
  // 本任务的停止令牌，协程迁移后仍关联同一停止状态。
  std::stop_token stop_token{};  // 从父任务或根任务停止源继承。
  cancellation_error_policy cancellation_policy{cancellation_error_policy::fatal_if_unobserved};
  external_scope_ref external_scope{};
  std::shared_ptr<cancellation_state> cancellation_owner{};
  // 当前优先级元数据；显式设置覆盖父任务，否则继承父任务。
  task_priority priority{task_priority::normal};
  // 记录 with_priority 是否明确设置过，继承上下文时据此保留子任务设置。
  bool priority_explicit{};  // 显式指定时保留，否则继承父任务的优先级。
  // 最近一次本任务条件让出检查的剩余预算镜像；真实预算属于本轮恢复的共享 TLS。
  // 属性继承/恢复不能把这个镜像写回 TLS，否则短子任务会意外重新取得额度。
  std::uint16_t budget{cooperative_budget_limit};
};

// 恢复本任务的 TLS；停止令牌复制赋值的相同状态快路径由标准库处理。
// 挂起迁移后 worker 已清空 TLS，仍然完整恢复本任务的停止令牌。
inline void restore_task_context(const task_context& context) noexcept {
  current_tracker = context.scope;
  current_stop_token = context.stop_token;
  current_cancellation_policy = context.cancellation_policy;
  current_external_scope = context.external_scope;
  current_cancellation_owner = context.cancellation_owner;
  // 协作预算由真实 scheduler resume 的作用域管理，恢复帧属性时绝不覆盖它。
}

class scoped_task_context {
 public:
  explicit scoped_task_context(const task_context& context) noexcept
      : previous_{.scope = current_tracker, .stop_token = current_stop_token,
                  .cancellation_policy = current_cancellation_policy,
                  .external_scope = current_external_scope,
                  .cancellation_owner = current_cancellation_owner} {
    restore_task_context(context);
  }
  scoped_task_context(const scoped_task_context&) = delete;
  scoped_task_context& operator=(const scoped_task_context&) = delete;
  ~scoped_task_context() { restore_task_context(previous_); }
 private:
  task_context previous_;
};
}  // namespace detail
}  // namespace faio
#endif
