/**
 * @file faio/detail/coroutine/composition/settle.hpp
 * @brief 等待原生任务并将完成结果转换为显式终态。
 */
#pragma once

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/composition/result.hpp"
#include "faio/detail/coroutine/task.hpp"
#include <coroutine>
#include <exception>
#include <type_traits>
#include <utility>

namespace faio {
namespace detail {
/** @brief 复用原生任务等待协议，在子帧回收前识别其异常与有效停止请求。 */
template <class T, class S>
struct settling_awaiter {
  using scheduler_type = S;
  typename task<T, S>::awaiter child;  ///< 原生等待器，独占子帧及其结果消费权。

  /** @brief 沿用原生等待器的就绪判定。 */
  bool await_ready() const noexcept { return child.await_ready(); }

  /** @brief 沿用直接子任务启动、受限调度与续体登记。 */
  template <class Parent>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Parent> parent) {
    return child.await_suspend(parent);
  }

  /**
   * @brief 读取终态并由原生等待器回收子帧。
   * @details 只有子帧记录的取消异常可成为 stopped。领取或包装值时抛出的异常属于 error，
   * 即使异常类型也是 operation_cancelled。
   */
  task_outcome<T> await_resume() {
    const auto handle = child.callee;
    const bool requested_on_failure = handle && handle.promise().exception
                                      && handle.promise().context.stop_token.stop_requested();
    try {
      if constexpr (std::is_void_v<T>) {
        child.await_resume();
        return task_outcome<T>::from_value();
      } else {
        return task_outcome<T>::from_value(child.await_resume());
      }
    } catch (const operation_cancelled&) {
      if (requested_on_failure)
        return task_outcome<T>::from_stopped(std::current_exception());
      return task_outcome<T>::from_error(std::current_exception());
    } catch (...) {
      return task_outcome<T>::from_error(std::current_exception());
    }
  }
};
}  // namespace detail

/**
 * @brief 惰性地消费一次原生任务，等待其完成并返回显式终态。
 * @details 停止请求本身不会提前交付结果。仅在子帧记录 operation_cancelled 且其有效停止令牌
 * 已收到请求时报告 stopped。该边界规则无法区分子任务内部抛出同类异常的所有来源。
 * @param child 交给包装任务的子任务，保持原静态调度器类型。
 * @return 沿用原生任务上下文的惰性观察任务。创建包装协程帧仍可能抛出分配失败。
 */
template <class T, class S>
task<task_outcome<T>, S> settle(task<T, S> child) {
  try {
    co_return co_await detail::settling_awaiter<T, S>{std::move(child).operator co_await()};
  } catch (...) {
    co_return task_outcome<T>::from_error(std::current_exception());
  }
}
}  // namespace faio
