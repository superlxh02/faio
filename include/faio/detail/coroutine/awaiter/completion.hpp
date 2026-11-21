/**
 * @file faio/detail/coroutine/awaiter/completion.hpp
 * @brief 保留具体调度器类型的单次完成令牌与 promise 匹配约束。
 */
#ifndef FAIO_DETAIL_COROUTINE_AWAITER_COMPLETION_HPP
#define FAIO_DETAIL_COROUTINE_AWAITER_COMPLETION_HPP

#include "faio/detail/coroutine/awaiter/state.hpp"
#include "faio/detail/coroutine/scheduler/resume_target.hpp"
#include <concepts>
#include <coroutine>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace faio::detail {
/**
 * @brief 单次完成令牌，持有操作状态；S 和 Operation 始终保留具体类型。
 * 回调在调用 complete 时交还结果和借用资源，此后不得再读写它们。
 * 令牌只能移动。销毁未完成的令牌不会取消操作或恢复等待者，接纳操作的一方仍须负责通知完成。
 */
template <class Operation, class S>
class awaiter_completion {
 public:
  using state_type = custom_awaiter_state<Operation>;

  /** @brief 构造完成令牌。 */
  awaiter_completion(std::shared_ptr<state_type> state,
                     basic_resume_target<S> scheduler,
                     std::coroutine_handle<> continuation) noexcept
      : state_(std::move(state)), scheduler_(scheduler), continuation_(continuation) {}

  /** @brief 禁止复制完成令牌。 */
  awaiter_completion(const awaiter_completion&) = delete;
  /** @brief 禁止复制赋值。 */
  awaiter_completion& operator=(const awaiter_completion&) = delete;
  /** @brief 移动完成令牌。 */
  awaiter_completion(awaiter_completion&&) noexcept = default;
  /** @brief 移动赋值。 */
  awaiter_completion& operator=(awaiter_completion&&) noexcept = default;

  /** @brief 用户自行订阅停止；停止请求不代替后端排空或完成通知。 */
  std::stop_token stop_token() const noexcept { return state_ ? state_->stop : std::stop_token{}; }

  /** @brief 发布操作完成状态。 */
  void complete() && noexcept {
    auto state = std::move(state_);
    if (!state)
      std::terminate();
    const auto scheduler = scheduler_;
    const auto continuation = continuation_;
    const auto previous = state->gate.exchange(state_type::completed, std::memory_order_acq_rel);
    if (previous == state_type::suspended) {
      try {
        scheduler.schedule(continuation);
      } catch (...) {
        std::terminate();
      }
    } else if (previous != state_type::registering) {
      std::terminate();
    }
  }

 private:
  std::shared_ptr<state_type> state_;
  basic_resume_target<S> scheduler_;
  std::coroutine_handle<> continuation_{};
};

template <class S, class Promise>
concept custom_awaiter_matching_scheduler =
    !has_task_scheduler_type<Promise> || std::same_as<S, typename Promise::scheduler_type>;
}  // namespace faio::detail

#endif
