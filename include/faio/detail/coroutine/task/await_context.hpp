/**
 * @file faio/detail/coroutine/task/await_context.hpp
 * @brief 标准 awaitable 转换及恢复任务上下文的静态适配。
 */
#ifndef FAIO_DETAIL_COROUTINE_TASK_AWAIT_CONTEXT_HPP
#define FAIO_DETAIL_COROUTINE_TASK_AWAIT_CONTEXT_HPP

#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/coroutine/scheduler/reference.hpp"
#include <type_traits>
#include <utility>

namespace faio::detail {
/**
 * @brief 把协程表达式转换成 awaiter。普通 awaitable 走原生协议；task 的
 * operator co_await 在这里取得独占所有权。此处只在编译期分派，不分配内存。
 */
template <class A>
decltype(auto) as_awaiter(A&& value) {
  if constexpr (requires { std::forward<A>(value).operator co_await(); }) {
    return std::forward<A>(value).operator co_await();
  } else if constexpr (requires { operator co_await(std::forward<A>(value)); }) {
    return operator co_await(std::forward<A>(value));
  } else {
    return std::forward<A>(value);
  }
}

/**
 * @brief 确定上下文适配器的存储类型，保留左值借用和无法构造值对象的右值引用。
 * @details 右值临时对象在完整 co_await 表达式内有效。具名对象和 operator co_await
 * 返回的引用仍由调用者保证有效期。
 * @tparam A 原生等待表达式转换得到的类型。
 */
template <class A>
using scoped_awaiter_storage_t =
    std::conditional_t<std::is_lvalue_reference_v<A>
                           || (std::is_rvalue_reference_v<A>
                               && !std::is_constructible_v<std::remove_cvref_t<A>, A>),
                       A,
                       std::remove_cvref_t<A>>;

/** @brief 标识库内 task 的原生等待器；通用等待器仍恢复完整上下文。 */
struct native_task_awaiter_tag {};

/**
 * @brief 给任意等待操作加一层任务上下文恢复适配，不引入堆分配。
 * inner 根据 scoped_awaiter_storage_t 持有值或借用原生等待器。借用只在完整表达式及原对象
 * 有效期内使用。
 */
template <class A, class S = default_scheduler>
struct scoped_awaiter {
  A inner; /**< 实际等待协议对象，await_ready/await_suspend/await_resume 都转发给它。 */
  const task_context*
      context; /**< 借用所属 promise 的上下文；父帧在本次等待结束前始终存活且地址稳定。 不复制 stop_token，避免每个 co_await 增减停止状态的原子引用计数。 */

  /** @brief 挂起前检查：保持底层等待操作的立即完成判定。 */
  decltype(auto) await_ready() { return inner.await_ready(); }

  /** @brief 挂起时：保持底层 await_suspend 的 bool/void/协程句柄返回语义。 */
  template <class H>
  decltype(auto) await_suspend(H h) {
    if constexpr (std::is_base_of_v<native_task_awaiter_tag, std::remove_cvref_t<A>>) {
      return inner.await_suspend_scoped(h);
    } else {
      return inner.await_suspend(h);
    }
  }

  /** @brief 恢复时：先重建本任务 TLS，再领取底层结果或让底层抛出异常。 */
  decltype(auto) await_resume() {
    if constexpr (std::is_base_of_v<native_task_awaiter_tag, std::remove_cvref_t<A>>) {
      return inner.await_resume_scoped(*context);
    } else {
      restore_task_context(*context);
      return inner.await_resume();
    }
  }
};

}  // namespace faio::detail
#endif
