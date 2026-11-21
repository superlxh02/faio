/**
 * @file faio/detail/coroutine/composition/with_stop_token.hpp
 * @brief 为局部任务接入独立的协作停止源。
 */
#pragma once

#include "faio/detail/coroutine/composition/settle.hpp"
#include <coroutine>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace faio {
namespace detail {
/** @brief 在已有父停止连接之外保有传入令牌的连接。 */
struct additional_stop_state : linked_stop_state {
  additional_stop_state(std::stop_token extra, const task_context& parent)
      : linked_stop_state(parent.stop_token, parent.cancellation_owner) {
    if (extra.stop_possible())
      additional_link.emplace(extra, forward{&source});
  }

  std::optional<std::stop_callback<forward>> additional_link;
};

/** @brief 复用原生交接和结果消费，仅在子任务发布前替换其停止源。 */
template <class T, class S, bool Observe>
struct with_stop_token_awaiter {
  using scheduler_type = S;
  using native_awaiter = typename task<T, S>::awaiter;
  using work_awaiter = std::conditional_t<Observe, settling_awaiter<T, S>, native_awaiter>;

  work_awaiter work;
  std::stop_token extra;

  bool await_ready() const noexcept { return work.await_ready(); }

  template <class Parent>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Parent> parent) {
    auto owner = std::make_shared<additional_stop_state>(extra, parent.promise().context);
    if constexpr (Observe)
      return work.child.await_suspend_impl(parent, owner);
    else
      return work.await_suspend_impl(parent, owner);
  }

  decltype(auto) await_resume() { return work.await_resume(); }
};
}  // namespace detail

/**
 * @brief 惰性地运行子任务，同时接受父令牌或传入令牌的停止请求。
 * @details 任一令牌收到请求时向子任务转发。任务继续协作执行，最终值或异常通过原生协议交付。
 * 子任务及继承该上下文的后代持有停止连接。需要显式终态时，可在局部边界内使用 settle。
 * @param token 要附加的停止令牌。
 * @param work 转入局部停止边界的用户任务。
 */
template <class T, class S>
task<T, S> with_stop_token(std::stop_token token, task<T, S> work) {
  if constexpr (std::is_void_v<T>) {
    co_await detail::with_stop_token_awaiter<T, S, false>{std::move(work).operator co_await(),
                                                          std::move(token)};
  } else {
    co_return co_await detail::with_stop_token_awaiter<T, S, false>{
        std::move(work).operator co_await(), std::move(token)};
  }
}
}  // namespace faio
