#ifndef FAIO_DETAIL_COROUTINE_THIS_CORO_HPP
#define FAIO_DETAIL_COROUTINE_THIS_CORO_HPP

#include "faio/detail/coroutine/task_context.hpp"
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <utility>

namespace faio {
// this_coro 返回轻量查询标记，由 task promise 的 await_transform 解释。
// 属性查询立即完成；让出操作由下面的 awaiter 把协程重新放入调度队列。
namespace this_coro {
// 无成员查询标记：co_await 时由 task promise 返回本任务 stop_token。
struct stop_token_t {};  // 查询协作停止令牌。

// 无成员查询标记：co_await 时取得借用的 scheduler_ref。
struct scheduler_t {};  // 查询所属调度器。

// 无成员查询标记：co_await 时动态读取当前 worker 标识。
struct worker_id_t {};  // 查询本次恢复所在的 worker。

// 无成员查询标记：co_await 时读取任务优先级元数据。
struct priority_t {};  // 查询任务优先级元数据。

// 无成员操作标记：co_await 时主动把当前协程重新入队。
struct yield_t {};  // 主动让出执行权。

// 无成员操作标记：co_await 时检查本任务的协作预算。
struct yield_if_needed_t {};

// 创建停止查询标记，调用本函数本身不查询或发停止请求。
inline constexpr stop_token_t stop_token() noexcept {
  return {};
}

// 创建调度器查询标记，等待它时才由 promise 提供当前任务属性。
inline constexpr scheduler_t scheduler() noexcept {
  return {};
}

// 创建 worker 查询标记，不缓存调用函数瞬间的线程编号。
inline constexpr worker_id_t worker_id() noexcept {
  return {};
}

// 创建优先级查询标记，返回的是任务元数据。
inline constexpr priority_t priority() noexcept {
  return {};
}

// 创建主动让出标记，只有 co_await 才会真正重新排队。
inline constexpr yield_t yield() noexcept {
  return {};
}

// 创建条件让出标记，本函数本身不修改预算。
inline constexpr yield_if_needed_t yield_if_needed() noexcept {
  return {};
}
}  // namespace this_coro

namespace detail {
// 保存属性快照的立即完成 awaiter，无挂起与调度成本。
template <class T>
struct query_awaiter {
  // promise 在 await_transform 中提供的属性副本，恢复时直接返回。
  T value;

  // 挂起前检查：属性查询已经就绪，始终立即完成。
  bool await_ready() const noexcept { return true; }

  // 挂起时：立即完成查询不会实际进入此路径，仅补齐协议。
  void await_suspend(std::coroutine_handle<>) const noexcept {}

  // 恢复时：返回先前保存的属性快照。
  T await_resume() noexcept { return std::move(value); }
};

// worker 编号随线程迁移而变化，必须动态读取，不能缓存为任务永久属性。
// 无成员动态查询器；不保存 worker_id，保证任务迁移后读取当前线程。
struct worker_id_awaiter {
  // 挂起前检查：属性查询已经就绪，始终立即完成。
  bool await_ready() const noexcept { return true; }

  // 挂起时：立即完成查询不会实际进入此路径，仅补齐协议。
  void await_suspend(std::coroutine_handle<>) const noexcept {}

  // 恢复时：读取当前 worker 的 TLS 标识，不读取旧线程的缓存。
  std::size_t await_resume() const noexcept { return ::faio::detail::current_worker_id(); }
};

// 主动让出操作：保存恢复上下文，挂起后让运行时重新安排执行。
struct yield_awaiter {
  scheduler_ref scheduler;            // 按值保存两指针调度借用，提交时不额外解引用 context。
  task_tracker* scope;                // 按值保存恢复时的外层任务组借用。
  const std::stop_token* stop_token;  // 借用 promise 内令牌，避免构造/析构令牌副本。

  // 挂起前检查：主动让出总需要重新排队。
  bool await_ready() const noexcept { return false; }

  // 把当前协程重新入队，让调度器执行其他就绪任务。
  // 挂起时：重新投递当前句柄，具体 worker 和执行时机由调度器决定。
  void await_suspend(std::coroutine_handle<> h) const {
    scheduler.schedule_cooperative_yield(
        h);  // 发布自身以后立即返回；公平 FIFO 与旧 custom 回退不变。
  }

  // 恢复时：重建当前任务组及停止令牌，再继续用户代码。
  void await_resume() const noexcept {
    // 恢复任务组与停止状态；相同令牌无需再修改引用计数。
    current_tracker = scope;
    current_stop_token = *stop_token;
  }
};

// 使用本轮实际恢复的共享预算；直接等待的所有父子 task 消耗同一份额度。
struct yield_if_needed_awaiter {
  // 借用当前 promise 的上下文，包括预算、调度器、任务组和停止令牌。
  task_context* context;

  // 挂起前检查：递减预算，非零时立即完成，归零时进入重新排队路径。
  bool await_ready() noexcept {
    // 已耗尽时直接挂起，避免回绕；帧中镜像不参与取得或重置额度。
    if (current_cooperative_budget != 0)
      --current_cooperative_budget;
    context->budget = current_cooperative_budget;  // 仅供上下文诊断的最近检查快照。
    return current_cooperative_budget != 0;
  }

  // 挂起时：重新投递当前句柄，具体 worker 和执行时机由调度器决定。
  void await_suspend(std::coroutine_handle<> h) const {
    context->scheduler.schedule_cooperative_yield(
        h);  // 预算耗尽后发布自身立即返回，不能重占快速槽。
  }

  // 恢复时：重置预算并恢复任务上下文；立即完成路径也会调用本接口。
  void await_resume() const noexcept {
    // 调度器按选中来源设置本轮额度；普通 fast
    // 延用同链余额，FIFO让出获得新额度。 未采用 poll scope
    // 的手工调度器仍在真正的耗尽让出后补充预算，保持兼容公平性。
    if (current_cooperative_budget == 0)
      current_cooperative_budget = cooperative_budget_limit;
    context->budget = current_cooperative_budget;
    // 只恢复任务组/停止属性，不用旧帧预算覆盖本次恢复的新额度。
    restore_task_context(*context);
  }
};
}  // namespace detail
}  // namespace faio
#endif
