/**
 * @file faio/detail/coroutine/lifecycle/policy.hpp
 * @brief 任务生命周期的状态、静态策略约束与回调适配。
 */
#ifndef FAIO_DETAIL_COROUTINE_LIFECYCLE_POLICY_HPP
#define FAIO_DETAIL_COROUTINE_LIFECYCLE_POLICY_HPP

#include <concepts>
#include <exception>
#include <functional>

namespace faio {
/** @brief 任务体退出原因；只观察状态和异常，不领取或移动任务结果。 */
struct task_exit {
  enum class kind { succeeded, failed, cancelled };

  kind status{kind::succeeded};
  std::exception_ptr exception{};
};

/** @brief 策略由包装任务按值拥有；before 可抛异常，after 不得抛异常。 */
template <class Policy>
concept task_lifecycle_policy =
    std::move_constructible<Policy> && requires(Policy& policy, const task_exit& exit) {
      { policy.before() } -> std::same_as<void>;
      { policy.after(exit) } noexcept -> std::same_as<void>;
    };

namespace detail {
/** @brief 便捷入口的两个静态回调须满足同一生命周期策略协议。 */
template <class Before, class After>
concept task_lifecycle_callback_pair =
    std::move_constructible<Before> && std::move_constructible<After>
    && requires(Before& before, After& after, const task_exit& exit) {
         { std::invoke(before) } -> std::same_as<void>;
         { std::invoke(after, exit) } noexcept -> std::same_as<void>;
       };

/** @brief 按值保存回调，并向包装协程提供统一的 before/after 接口。 */
template <class Before, class After>
  requires task_lifecycle_callback_pair<Before, After>
struct task_lifecycle_callbacks {
  Before before_callback;
  After after_callback;

  /** @brief 调用任务开始前的回调。 */
  void before() { std::invoke(before_callback); }

  /** @brief 将任务退出状态传给结束回调；回调不得抛出异常。 */
  void after(const task_exit& exit) noexcept { std::invoke(after_callback, exit); }
};
}  // namespace detail
}  // namespace faio
#endif
