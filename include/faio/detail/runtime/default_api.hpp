/**
 * @file faio/detail/runtime/default_api.hpp
 * @brief 协程运行时、配置与提交入口，不包含文件系统、网络或流算法模块。
 */
#ifndef FAIO_DETAIL_RUNTIME_DEFAULT_API_HPP
#define FAIO_DETAIL_RUNTIME_DEFAULT_API_HPP

#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/runtime/config_builder.hpp" /* IWYU pragma: export */
#include "faio/detail/coroutine.hpp"              /* IWYU pragma: export */
#include "faio/detail/runtime/context.hpp"
#include "faio/detail/runtime/default.hpp"
#include "faio/detail/runtime/static.hpp" /* IWYU pragma: export */
#include <concepts>
#include <stdexcept>
#include <utility>

namespace faio {
/**
 * @brief 默认运行时入口：worker 内直接使用 TLS 调度器；任意外部线程通过
 * 进程默认运行时提交，不要求调用线程曾经创建过运行时。
 */
template <class T, class S>
join_handle<T, S> spawn(task<T, S> child) {
  if (auto scheduler = detail::current_scheduler_for<S>())
    return join_handle<T, S>{
        detail::schedule_observed_task(scheduler,
                                       std::move(child),
                                       ::faio::detail::current_task_context().scope,
                                       ::faio::detail::current_task_context().stop_token)};
  if constexpr (!std::same_as<S, default_scheduler>)
    throw std::logic_error("自定义静态调度器需要当前执行线程绑定");
  else
    return runtime::detail::default_service().with_context(
        [&](runtime::detail::runtime_context& ctx) -> join_handle<T, S> {
          return join_handle<T, S>{detail::schedule_observed_task(
              ctx.scheduler(), std::move(child), nullptr, {}, ctx.task_lifetime())};
        });
}

/** @brief 分离提交任务，不返回结果句柄；未处理异常将终止进程。 */
template <class T, class S>
void spawn_detached(task<T, S> child) {
  if (auto scheduler = detail::current_scheduler_for<S>()) {
    detail::schedule_unobserved_task(
        scheduler, std::move(child), ::faio::detail::current_task_context().scope);
    return;
  }
  if constexpr (!std::same_as<S, default_scheduler>)
    throw std::logic_error("自定义静态调度器需要当前执行线程绑定");
  else
    runtime::detail::default_service().with_context([&](runtime::detail::runtime_context& ctx) {
      detail::schedule_unobserved_task(
          ctx.scheduler(), std::move(child), nullptr, ctx.task_lifetime());
    });
}

namespace detail {
/** @brief 向当前或默认阻塞池提交函数，并绑定指定调度器类型。 */
template <class S, class F>
auto spawn_blocking_for(F&& function) {
  if (auto* pool = current_blocking_pool())
    return runtime::detail::submit_blocking_task<S>(
        *pool,
        std::forward<F>(function),
        ::faio::detail::current_task_context().scope,
        ::faio::detail::current_task_context().stop_token,
        current_task_lifetime(),
        current_scheduler_for<S>());
  if constexpr (!std::same_as<S, default_scheduler>)
    throw std::logic_error("自定义静态调度器需要当前执行线程绑定");
  else
    return runtime::detail::default_service().with_context(
        [&](runtime::detail::runtime_context& ctx) {
          return ctx.template submit_blocking<S>(std::forward<F>(function));
        });
}
}  // namespace detail

/**  @brief 将同步函数提交到用户阻塞池；等待只挂起协程。 */
template <class F>
auto spawn_blocking(F&& function) {
  return detail::spawn_blocking_for<default_scheduler>(std::forward<F>(function));
}

/** @brief 向阻塞池提交同步函数，使用指定静态调度器交付结果。 */
template <coroutine_scheduler S, class F>
auto spawn_blocking(F&& function) {
  return detail::spawn_blocking_for<S>(std::forward<F>(function));
}

/** @brief 提交根包装并同步等待其任务组结束，随后领取结果或传播异常。 */
template <typename T, class S>
inline auto block_on(task<T, S> t) -> T {
  static_assert(std::same_as<S, default_scheduler>,
                "默认运行时只接收其具体调度器类型；自定义调度器需显式驱动");
  if (detail::on_runtime_worker())
    throw std::logic_error("不能在 worker 线程调用 block_on；请 co_await task");
  return runtime::detail::default_service().with_context(
      [&](runtime::detail::runtime_context& ctx) -> T { return ctx.block_on(std::move(t)); });
}

}  // namespace faio
#endif
