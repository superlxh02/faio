/**
 * @file faio/detail/coroutine/lifecycle/task_adapter.hpp
 * @brief 手动配置任务生命周期的策略所有权、退出守卫和包装协程。
 */
#ifndef FAIO_DETAIL_COROUTINE_LIFECYCLE_TASK_ADAPTER_HPP
#define FAIO_DETAIL_COROUTINE_LIFECYCLE_TASK_ADAPTER_HPP

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/lifecycle/policy.hpp"
#include "faio/detail/coroutine/task/type.hpp"
#include <concepts>
#include <exception>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace faio {
namespace detail {
/** @brief 位于协程体内，未启动就销毁帧时不会构造；after 在结果写入 promise 后执行。 */
template <task_lifecycle_policy Policy>
class task_lifecycle_exit_guard {
 public:
  /** @brief 借用退出时调用的生命周期策略。 */
  explicit task_lifecycle_exit_guard(Policy& policy) noexcept : policy_(policy) {}

  /** @brief 禁止复制退出守卫。 */
  task_lifecycle_exit_guard(const task_lifecycle_exit_guard&) = delete;
  /** @brief 禁止复制赋值。 */
  task_lifecycle_exit_guard& operator=(const task_lifecycle_exit_guard&) = delete;

  /** @brief 退出作用域时向策略传递任务退出状态。 */
  ~task_lifecycle_exit_guard() { policy_.after(exit_); }

  /** @brief 记录任务退出原因及异常。 */
  void fail(task_exit::kind status, std::exception_ptr exception) noexcept {
    exit_.status = status;
    exit_.exception = std::move(exception);
  }

 private:
  Policy& policy_;
  task_exit exit_{};
};

/** @brief 参数按值进入帧；该协程不借用调用 with_lifecycle 的 task 或临时策略对象。 */
template <class T, class S, class Factory, task_lifecycle_policy Policy>
  requires std::same_as<std::invoke_result_t<Factory&>, task<T, S>>
task<T, S> with_lifecycle_factory(Factory factory, Policy policy) {
  task_lifecycle_exit_guard guard{policy};
  try {
    policy.before();
  } catch (...) {
    guard.fail(task_exit::kind::failed, std::current_exception());
    throw;
  }

  bool returning_result = false;
  try {
    auto inner = std::invoke(factory);
    if constexpr (std::is_void_v<T>) {
      co_await std::move(inner);
      co_return;
    } else {
      T result = co_await std::move(inner);
      returning_result = true;
      co_return std::move(result);
    }
  } catch (const operation_cancelled&) {
    guard.fail(returning_result ? task_exit::kind::failed : task_exit::kind::cancelled,
               std::current_exception());
    throw;
  } catch (...) {
    guard.fail(task_exit::kind::failed, std::current_exception());
    throw;
  }
}
}  // namespace detail

/** @brief 成员定义在完整 task 之后提供；按值捕获与原包装协程协议保持一致。 */
template <class T, class S>
template <class Policy>
  requires task_lifecycle_policy<std::decay_t<Policy>>
           && std::constructible_from<std::decay_t<Policy>, Policy&&>
inline task<T, S> task<T, S>::with_lifecycle(Policy&& policy) && {
  if (!handle_)
    throw std::logic_error("不能为空 task 配置生命周期策略");
  const auto context = handle_.promise().context;
  const auto scheduling = handle_.promise().scheduling_options;
  handle_.promise().scheduling_options = {};
  auto factory = [inner = std::move(*this)]() mutable {
    return std::move(inner);
  };
  auto wrapped = detail::with_lifecycle_factory<T, S>(
      std::move(factory), std::decay_t<Policy>(std::forward<Policy>(policy)));
  wrapped.handle_.promise().context = context;
  wrapped.handle_.promise().scheduling_options = scheduling;
  return wrapped;
}

/** @brief 回调便捷入口复用同一个静态策略包装，不新增分派层。 */
template <class T, class S>
template <class Before, class After>
  requires detail::task_lifecycle_callback_pair<std::decay_t<Before>, std::decay_t<After>>
           && std::constructible_from<std::decay_t<Before>, Before&&>
           && std::constructible_from<std::decay_t<After>, After&&>
inline task<T, S> task<T, S>::with_lifecycle(Before&& before, After&& after) && {
  return std::move(*this).with_lifecycle(
      detail::task_lifecycle_callbacks<std::decay_t<Before>, std::decay_t<After>>{
          std::forward<Before>(before), std::forward<After>(after)});
}
}  // namespace faio
#endif
