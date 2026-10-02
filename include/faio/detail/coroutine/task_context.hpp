#ifndef FAIO_DETAIL_COROUTINE_TASK_CONTEXT_HPP
#define FAIO_DETAIL_COROUTINE_TASK_CONTEXT_HPP

#include "faio/detail/coroutine/execution_thread.hpp"
#include "faio/detail/coroutine/task_tracker.hpp"
#include <cstdint>
#include <stop_token>

namespace faio {
// 当前为上下文元数据；具体调度队列尚未按优先级分层。
// 优先级元数据：low 低、normal 默认、high 高；当前队列仍未按它分层调度。
enum class task_priority : unsigned char { low, normal, high };

namespace detail {
// 当前协程的停止令牌。worker 每次执行根协程时清空，task 的 awaiter
// 恢复用户代码前重建；挂起后的任务上下文以 promise 中的副本为准。
// 当前用户协程的协作停止观察端，普通线程或无停止源的任务默认为空。
inline thread_local std::stop_token current_stop_token{};

// 父任务在 await_suspend 传递调度器、停止请求和作用域。worker_id 是动态
// 查询，因为可移动任务每次恢复可能落在不同的 worker。
// 保存于用户 task promise 的执行属性集合。
// 父子直接 co_await 时按值继承；其中的调度器与 tracker 指针只借用外部对象。
struct task_context {
  // 当前任务所属的根任务计数组，恢复 TLS 后供派生 spawn 登记。
  ::faio::detail::task_tracker* scope{}; // 借用任务组，供派生根任务登记。
  // 所属运行时的轻量调度接口，让出与等待唤醒使用它。
  scheduler_ref scheduler{}; // 借用目标调度器，不延长运行时生命周期。
  // 本任务的停止令牌，协程迁移后仍关联同一停止状态。
  std::stop_token stop_token{}; // 从父任务或根任务停止源继承。
  // 当前优先级元数据；显式设置覆盖父任务，否则继承父任务。
  task_priority priority{task_priority::normal};
  // 记录 with_priority 是否明确设置过，继承上下文时据此保留子任务设置。
  bool priority_explicit{}; // 显式指定时保留，否则继承父任务的优先级。
  // 条件让出的可变预算，awaiter 借用该成员地址而无需额外状态分配。
  std::uint16_t budget{64}; // yield_if_needed 使用的协作让出预算。
};

// 恢复本任务的 TLS；停止令牌复制赋值的相同状态快路径由标准库处理。
// 挂起迁移后 worker 已清空 TLS，仍然完整恢复本任务的停止令牌。
inline void restore_task_context(const task_context& context) noexcept {
  current_tracker = context.scope;
  current_stop_token = context.stop_token;
}

} // namespace detail
} // namespace faio
#endif
