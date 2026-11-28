/**
 * @file faio/detail/coroutine/awaiter/adapter.hpp
 * @brief 标准协程等待器、显式工厂和 CRTP 适配接口。
 */
#ifndef FAIO_DETAIL_COROUTINE_AWAITER_ADAPTER_HPP
#define FAIO_DETAIL_COROUTINE_AWAITER_ADAPTER_HPP

#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/coroutine/awaiter/operation.hpp"
#include "faio/detail/coroutine/awaiter/completion.hpp"
#include "faio/detail/coroutine/context/task_context.hpp"
#include <stdexcept>

namespace faio {
/**
 * @brief 拥有用户操作的一次性等待器；构造、移动和调度器重绑定均不启动操作。
 * 挂起期间不得从外部销毁父协程。借用的资源必须从实际异步操作开始一直有效到 complete 调用。
 */
template <class Operation, class S = default_scheduler>
class coroutine_awaiter {
  using state_type = detail::custom_awaiter_state<Operation>;

 public:
  using scheduler_type = S;
  using completion_type = detail::awaiter_completion<Operation, S>;
  static_assert(await_operation_for<Operation, completion_type>,
                "操作需要 noexcept void on_suspend(completion) 和返回 void/非引用值的 on_resume()");
  using result_type = decltype(std::declval<Operation&&>().on_resume());

  /** @brief 构造等待器，接管操作状态。 */
  template <class O>
    requires std::constructible_from<Operation, O&&>
  explicit coroutine_awaiter(O&& operation)
      : state_(std::make_shared<state_type>(std::forward<O>(operation))) {}

  /** @brief 禁止复制等待器。 */
  coroutine_awaiter(const coroutine_awaiter&) = delete;
  /** @brief 禁止复制赋值。 */
  coroutine_awaiter& operator=(const coroutine_awaiter&) = delete;
  /** @brief 移动等待器。 */
  coroutine_awaiter(coroutine_awaiter&&) noexcept = default;
  /** @brief 移动赋值。 */
  coroutine_awaiter& operator=(coroutine_awaiter&&) noexcept = default;

  /** @brief 绑定等待器的静态恢复调度器。 */
  template <class Target>
  auto rebind_scheduler(scheduler_ref_for<Target> scheduler) & {
    if (!state_ || state_->gate.load(std::memory_order_acquire) != state_type::IDLE)
      throw std::logic_error("不能重新绑定已使用的用户等待器");
    return coroutine_awaiter<Operation, Target>{std::exchange(state_, {}), scheduler};
  }

  /** @brief 绑定等待器的静态恢复调度器。 */
  template <class Target>
  auto rebind_scheduler(scheduler_ref_for<Target> scheduler) && {
    return this->template rebind_scheduler<Target>(scheduler);
  }

  /** @brief 检查等待条件是否已满足。 */
  bool await_ready() const noexcept { return false; }

  /** @brief 登记挂起协程的恢复路径。 */
  template <class Promise>
    requires detail::custom_awaiter_matching_scheduler<S, Promise>
  bool await_suspend(std::coroutine_handle<Promise> continuation) {
    auto state = state_;
    if (!state)
      throw std::logic_error("用户等待器只能等待一次");
    auto scheduler = scheduler_;
    if constexpr (detail::has_task_scheduler_type<Promise>)
      scheduler = detail::require_current_scheduler_for<S>();
    else if (!scheduler)
      scheduler = detail::current_scheduler_for<S>();
    if (!scheduler)
      throw std::logic_error("用户等待器需要有效的静态调度器");
    const auto target = detail::capture_resume_target(scheduler);

    unsigned char idle = state_type::IDLE;
    if (!state->gate.compare_exchange_strong(
            idle, state_type::REGISTERING, std::memory_order_acq_rel))
      throw std::logic_error("用户等待器只能等待一次");

    const bool matching_thread = detail::current_scheduler_matches(scheduler);
    auto* scope = matching_thread ? ::faio::detail::current_task_context().scope : nullptr;
    state->stop =
        matching_thread ? ::faio::detail::current_task_context().stop_token : std::stop_token{};
    constexpr bool has_parent_context =
        requires {
          { continuation.promise().context.scope } -> std::same_as<detail::task_tracker*&>;
          { continuation.promise().context.stop_token } -> std::same_as<std::stop_token&>;
        } && detail::has_task_scheduler_type<Promise>
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
        && detail::execution_awaiter_context<Promise>
#endif
        ;
    if constexpr (has_parent_context) {
      scope = continuation.promise().context.scope;
      state->stop = continuation.promise().context.stop_token;
    }
    state->lifetime.capture(
        scope,
        matching_thread ? detail::current_task_lifetime() : task_lifetime_ref{}
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
        ,
        detail::awaiter_execution_scope<has_parent_context>(continuation, matching_thread)
#endif
    );

    state->operation.on_suspend(completion_type{state, target, continuation});
    unsigned char registering = state_type::REGISTERING;
    return state->gate.compare_exchange_strong(
        registering, state_type::SUSPENDED, std::memory_order_acq_rel);
  }

  /** @brief 取得等待结果。 */
  decltype(auto) await_resume() {
    auto state = std::exchange(state_, {});
    if (!state || state->gate.load(std::memory_order_acquire) != state_type::COMPLETED)
      throw std::logic_error("用户等待器尚未完成或已经被消费");
    return std::move(state->operation).on_resume();
  }

 private:
  template <class, class>
  friend class coroutine_awaiter;

  /** @brief 构造等待器，接管操作状态。 */
  coroutine_awaiter(std::shared_ptr<state_type> state, scheduler_ref_for<S> scheduler) noexcept
      : state_(std::move(state)), scheduler_(scheduler) {}

  std::shared_ptr<state_type> state_;
  scheduler_ref_for<S> scheduler_;
};

/** @brief 按值持有满足两阶段协议的用户类对象，结果类型由 on_resume 推导。 */
template <class Operation>
  requires await_operation<Operation>
           && std::constructible_from<std::remove_cvref_t<Operation>, Operation&&>
[[nodiscard]] auto make_awaiter(Operation&& operation) {
  return coroutine_awaiter<std::remove_cvref_t<Operation>>{std::forward<Operation>(operation)};
}

/** @brief 可选的无状态 CRTP 适配接口；仅接受右值，所有路径共用 make_awaiter。 */
template <class Derived>
struct awaiter_facade {
  /** @brief 构造协程等待适配器。 */
  [[nodiscard]] auto awaiter(this Derived&& self) { return faio::make_awaiter(std::move(self)); }

  /** @brief 构造协程等待器。 */
  [[nodiscard]] auto operator co_await(this Derived&& self) {
    return faio::make_awaiter(std::move(self));
  }

  /** @brief task 在 operator co_await 转换之前选择调度器；直接等待派生类也必须保留 S。 */
  template <class Target>
  auto rebind_scheduler(this Derived&& self, scheduler_ref_for<Target> scheduler) {
    return faio::make_awaiter(std::move(self)).template rebind_scheduler<Target>(scheduler);
  }
};
}  // namespace faio

#endif
