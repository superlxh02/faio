/**
 * @file faio/detail/coroutine/composition/with_cleanup.hpp
 * @brief 提供任务体与独立停止上下文中的异步清理组合。
 */
#pragma once

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/composition/settle.hpp"
#include "faio/detail/coroutine/context/context_scope.hpp"
#include "faio/detail/coroutine/lifecycle/policy.hpp"
#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace faio {
/** @brief 任务体退出和清理均产生异常，按发生阶段分别保留原异常。 */
class cleanup_error : public std::exception {
 public:
  /** @brief 保存任务体与清理的原始异常。 */
  cleanup_error(std::exception_ptr primary, std::exception_ptr secondary) noexcept
      : primary_(std::move(primary)), secondary_(std::move(secondary)) {}

  /** @brief 任务体的失败或协作取消异常。 */
  std::exception_ptr primary() const noexcept { return primary_; }

  /** @brief 清理工厂或清理任务的异常。 */
  std::exception_ptr secondary() const noexcept { return secondary_; }

  /** @brief 返回双失败的简要说明。 */
  const char* what() const noexcept override { return "任务体和清理均失败"; }

 private:
  std::exception_ptr primary_;    ///< 任务体的原始退出异常。
  std::exception_ptr secondary_;  ///< 清理的原始退出异常。
};

namespace detail {
/** @brief 等待清理任务，在交接续体前安装独立的停止状态。 */
template <class S>
struct cleanup_awaiter {
  using scheduler_type = S;

  typename task<void, S>::awaiter child;          ///< 清理帧的唯一所有者。
  std::shared_ptr<linked_stop_state> stop_owner;  ///< 清理任务及其后代使用的停止源。

  /** @brief 沿用原生任务的就绪判定。 */
  bool await_ready() const noexcept { return child.await_ready(); }

  /** @brief 保留父续体、原有调度策略和上下文，只替换清理子任务的停止状态。 */
  template <class Parent>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Parent> parent) {
    return child.await_suspend_impl(parent, stop_owner);
  }

  /** @brief 采用原生结果消费与清理帧回收协议，传播清理异常。 */
  void await_resume() { child.await_resume(); }
};

/** @brief 将结果状态转换为退出记录，供清理回调在整个等待期间借用。 */
template <class T>
task_exit cleanup_body_exit(const task_outcome<T>& outcome) noexcept {
  switch (outcome.status()) {
    case task_outcome_status::VALUE:
      return {};
    case task_outcome_status::ERROR_VALUE:
      return {task_exit::kind::FAILED, outcome.exception()};
    case task_outcome_status::STOPPED:
      return {task_exit::kind::CANCELLED, outcome.exception()};
  }
  std::terminate();
}

/** @brief 同步调用清理工厂，在返回清理任务前销毁线程上下文守卫。 */
template <class Cleanup>
auto invoke_cleanup_factory(Cleanup& cleanup,
                            const task_exit& exit,
                            const std::shared_ptr<linked_stop_state>& stop_owner) {
  auto context = capture_task_context();
  context.stop_token = stop_owner->source.get_token();
  context.cancellation_owner = stop_owner;
  scoped_task_context active{context};
  return std::invoke(cleanup, exit);
}
}  // namespace detail

/**
 * @brief 惰性执行任务体，并在同一任务边界内等待清理恰好执行一次。
 * @details 清理工厂及其任务使用新建的独立停止源。清理任务可以挂起和等待异步操作。
 * 发生双失败时抛 cleanup_error；只有一侧失败时传播该侧原异常。清理完成后才交付结果。
 * 清理完成后向等待方交付值时发生的移动异常按原生 task 规则传播，清理只观察已完成的任务体。
 * 同步主体工厂抛出 operation_cancelled 时，仅在主体有效令牌已收到请求时记录取消。
 * 清理工厂收到的退出记录只借用到清理任务退出，需排空借用该记录的后代。
 * @param body 启动时调用的任务体工厂。
 * @param cleanup 接收任务体退出记录并返回同静态调度器 task<void, S> 的清理工厂。
 */
template <class body_type, class Cleanup, class work_type = std::invoke_result_t<body_type&>>
  requires std::same_as<work_type,
                        task<typename work_type::value_type, typename work_type::scheduler_type>>
           && std::same_as<std::invoke_result_t<Cleanup&, const task_exit&>,
                           task<void, typename work_type::scheduler_type>>
work_type with_cleanup(body_type body, Cleanup cleanup) {
  using T = typename work_type::value_type;
  using S = typename work_type::scheduler_type;
  const auto cleanup_stop = std::make_shared<detail::linked_stop_state>(std::stop_token{}, nullptr);

  const auto body_stop = co_await this_coro::stop_token();
  std::optional<task_outcome<T>> result;
  task_exit exit;
  try {
    auto work = std::invoke(body);
    result.emplace(co_await detail::settling_awaiter<T, S>{std::move(work).operator co_await()});
    exit = detail::cleanup_body_exit(*result);
  } catch (const operation_cancelled&) {
    const auto status =
        body_stop.stop_requested() ? task_exit::kind::CANCELLED : task_exit::kind::FAILED;
    exit = {status, std::current_exception()};
  } catch (...) {
    exit = {task_exit::kind::FAILED, std::current_exception()};
  }

  try {
    auto work = detail::invoke_cleanup_factory(cleanup, std::as_const(exit), cleanup_stop);
    co_await detail::cleanup_awaiter<S>{std::move(work).operator co_await(), cleanup_stop};
  } catch (...) {
    if (exit.exception)
      throw cleanup_error(exit.exception, std::current_exception());
    throw;
  }

  if (exit.exception)
    std::rethrow_exception(exit.exception);
  if constexpr (std::is_void_v<T>)
    co_return;
  else
    co_return result->take_value();
}
}  // namespace faio
