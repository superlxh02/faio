/**
 * @file faio/detail/coroutine/context/this_coro.hpp
 * @brief 协程帧、任务上下文与并发组合：当前协程查询与协作让出。
 */
#ifndef FAIO_DETAIL_COROUTINE_THIS_CORO_HPP
#define FAIO_DETAIL_COROUTINE_THIS_CORO_HPP

#include "faio/detail/coroutine/context/execution_binding.hpp"
#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/coroutine/scheduler/resume_target.hpp"
#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <utility>

namespace faio {
/**
 * @brief this_coro 返回轻量查询标记，由 task promise 的 await_transform 解释。
 * 属性查询立即完成；让出操作由下面的 awaiter 把协程重新放入调度队列。
 */
namespace this_coro {
/**
 * @brief 查询协作停止令牌的无成员标记，co_await 时由 task promise 返回本任务的 stop_token。
 */
struct stop_token_t {};

/**
 * @brief 查询当前执行线程调度器的无成员标记，co_await 时取得借用的 scheduler_ref。
 */
struct scheduler_t {};

/**
 * @brief 查询本次恢复所在工作线程的无成员标记，co_await 时动态读取当前 worker 标识。
 */
struct worker_id_t {};

/**
 * @brief 主动让出执行权的无成员标记，co_await 时将当前协程重新入队。
 */
struct yield_t {};

/** @brief 无成员操作标记：co_await 时检查本任务的协作预算。 */
struct yield_if_needed_t {};

/** @brief 创建停止查询标记；调用本函数不会查询停止状态或发出停止请求。 */
inline constexpr stop_token_t stop_token() noexcept {
  return {};
}

/** @brief 创建调度器查询标记，等待它时才从当前线程绑定读取。 */
inline constexpr scheduler_t scheduler() noexcept {
  return {};
}

/** @brief 创建 worker 查询标记，不缓存调用函数瞬间的线程编号。 */
inline constexpr worker_id_t worker_id() noexcept {
  return {};
}

/** @brief 创建主动让出标记，只有 co_await 才会真正重新排队。 */
inline constexpr yield_t yield() noexcept {
  return {};
}

/** @brief 创建条件让出标记，本函数本身不修改预算。 */
inline constexpr yield_if_needed_t yield_if_needed() noexcept {
  return {};
}
}  // namespace this_coro

namespace detail {
/** @brief 保存属性快照的立即完成 awaiter，无挂起与调度成本。 */
template <class T>
struct query_awaiter {
  T value; /**< promise 在 await_transform 中提供的属性副本，恢复时直接返回。 */

  /** @brief 挂起前检查：属性查询已经就绪，始终立即完成。 */
  bool await_ready() const noexcept { return true; }

  /** @brief 挂起时：立即完成查询不会实际进入此路径，仅补齐协议。 */
  void await_suspend(std::coroutine_handle<>) const noexcept {}

  /** @brief 恢复时：返回先前保存的属性快照。 */
  T await_resume() noexcept { return std::move(value); }
};

/**
 * @brief 动态查询当前工作线程编号，不保存 worker_id。
 * 任务迁移后编号可能改变，因此不能将它缓存为任务的永久属性。
 */
struct worker_id_awaiter {
  /** @brief 挂起前检查：属性查询已经就绪，始终立即完成。 */
  bool await_ready() const noexcept { return true; }

  /** @brief 挂起时：立即完成查询不会实际进入此路径，仅补齐协议。 */
  void await_suspend(std::coroutine_handle<>) const noexcept {}

  /** @brief 恢复时：读取当前 worker 的 TLS 标识，不读取旧线程的缓存。 */
  std::size_t await_resume() const noexcept { return ::faio::detail::current_worker_id(); }
};

/** @brief 主动让出操作：保存恢复上下文，挂起后让运行时重新安排执行。 */
template <class S>
struct basic_yield_awaiter {
  basic_resume_target<S> scheduler;  /**< 捕获调度器及有效约束，恢复目标不依赖完成线程 TLS。 */
  task_tracker* scope;               /**< 按值保存恢复时的外层任务组借用。 */
  const std::stop_token* stop_token; /**< 借用 promise 内的令牌，避免构造和析构令牌副本。 */

  /** @brief 挂起前检查：主动让出总需要重新排队。 */
  bool await_ready() const noexcept { return false; }

  /**
   * @brief 把当前协程重新入队，让调度器执行其他就绪任务。
   * 挂起时：重新投递当前句柄，具体 worker 和执行时机由调度器决定。
   */
  void await_suspend(std::coroutine_handle<> h) const { scheduler.schedule_cooperative_yield(h); }

  /** @brief 恢复时：重建当前任务组及停止令牌，再继续用户代码。 */
  void await_resume() const noexcept {
    current_thread_context().task_ctx.scope = scope;
    current_thread_context().task_ctx.stop_token = *stop_token;
  }
};

/** @brief 使用本轮实际恢复的共享预算；直接等待的所有父子 task 消耗同一份额度。 */
template <class S>
struct basic_yield_if_needed_awaiter {
  const task_context* context; /**< 借用当前 promise 的任务属性；预算与调度器不保存在任务帧中。 */

  /** @brief 挂起前检查：递减预算，非零时立即完成，归零时进入重新排队路径。 */
  bool await_ready() noexcept {
    auto& remaining = current_thread_context().poll.remaining;
    if (remaining != 0)
      --remaining;
    return remaining != 0;
  }

  /** @brief 挂起时：重新投递当前句柄，具体 worker 和执行时机由调度器决定。 */
  void await_suspend(std::coroutine_handle<> h) const {
    capture_current_resume_target<S>().schedule_cooperative_yield(h);
  }

  /** @brief 恢复时：重置预算并恢复任务上下文；立即完成路径也会调用本接口。 */
  void await_resume() const noexcept {
    auto& remaining = current_thread_context().poll.remaining;
    if (remaining == 0)
      remaining = cooperative_budget_limit;
    restore_task_context(*context);
  }
};
}  // namespace detail
}  // namespace faio
#endif
