/**
 * @file faio/detail/coroutine/scheduler/resume_target.hpp
 * @brief 挂起时捕获的静态调度借用及有效调度约束。
 */
#pragma once

#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/coroutine/scheduler/policy.hpp"
#include "faio/detail/coroutine/context/execution_binding.hpp"
#include "faio/detail/runtime/common/resume_target.hpp"
#include <concepts>
#include <coroutine>
#include <stdexcept>
#include <type_traits>

namespace faio::detail {
enum class resume_submission { normal, io, yield, cooperative_yield };

/** @brief 无需容量预留的恢复占位状态。 */
struct empty_resume_reservation {};

template <class S>
using resume_reservation_for = std::conditional_t<std::same_as<S, default_scheduler>,
                                                  ::faio::runtime::detail::resume_reservation,
                                                  empty_resume_reservation>;

/** @brief 借用调度器并按值保存恢复约束；完成线程不读取其他任务的策略 TLS。 */
template <class S>
class basic_resume_target : private resume_reservation_for<S> {
  using reservation_state = resume_reservation_for<S>;

 public:
  using scheduler_type = S;

  /** @brief 构造静态调度恢复目标。 */
  basic_resume_target() noexcept = default;

  /** @brief 构造静态调度恢复目标。 */
  basic_resume_target(basic_scheduler_ref<S> scheduler, task_scheduling_policy policy = {})
      : scheduler_(scheduler), policy_(policy) {
    validate();
  }

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return static_cast<bool>(scheduler_); }

  /** @brief 读取任务调度策略。 */
  const task_scheduling_policy& policy() const noexcept { return policy_; }

  /** @brief 受限任务在开始异步等待前预留恢复容量；容量已满时，尚未接纳该等待的生命周期责任。 */
  void reserve() {
    if constexpr (std::same_as<S, default_scheduler>)
      static_cast<reservation_state&>(*this).reserve(scheduler_, policy_);
  }

  /** @brief 提交普通就绪任务。 */
  void schedule(std::coroutine_handle<> handle) const { submit(handle, resume_submission::normal); }

  /** @brief 提交 I/O 完成后的就绪任务。 */
  void schedule_io(std::coroutine_handle<> handle) const { submit(handle, resume_submission::io); }

  /** @brief 重新提交主动让出的协程。 */
  void schedule_yield(std::coroutine_handle<> handle) const {
    submit(handle, resume_submission::yield);
  }

  /** @brief 重新提交预算耗尽的协程。 */
  void schedule_cooperative_yield(std::coroutine_handle<> handle) const {
    submit(handle, resume_submission::cooperative_yield);
  }

 private:
  /** @brief 校验任务调度策略与调度器能力。 */
  void validate() const {
    if constexpr (!std::same_as<S, default_scheduler>) {
      if (policy_.binds_core()) {
        if constexpr (!requires(S& scheduler, std::coroutine_handle<> handle) {
                        { scheduler.enqueue_on(handle, std::size_t{}) } -> std::same_as<void>;
                      })
          throw std::logic_error("当前调度器不支持固定 worker 恢复");
      } else if (policy_.no_stealing) {
        if constexpr (!requires(S& scheduler, std::coroutine_handle<> handle) {
                        { scheduler.enqueue_no_stealing(handle) } -> std::same_as<void>;
                      })
          throw std::logic_error("当前调度器不支持不可窃取任务");
      }
    }
  }

  /** @brief 提交已准备的工作。 */
  void submit(std::coroutine_handle<> handle, resume_submission submission) const {
    if (!scheduler_)
      throw std::logic_error("没有可用的调度器");
    if constexpr (std::same_as<S, default_scheduler>) {
      static_cast<const reservation_state&>(*this).submit(scheduler_, policy_, handle, submission);
    } else {
      auto& scheduler = *scheduler_.native_handle();
      if (policy_.binds_core()) {
        if constexpr (requires { scheduler.enqueue_on(handle, policy_.bound_worker); })
          scheduler.enqueue_on(handle, policy_.bound_worker);
        else
          throw std::logic_error("当前调度器不支持固定 worker 恢复");
        return;
      }
      if (policy_.no_stealing) {
        if constexpr (requires { scheduler.enqueue_no_stealing(handle); })
          scheduler.enqueue_no_stealing(handle);
        else
          throw std::logic_error("当前调度器不支持不可窃取任务");
        return;
      }
      switch (submission) {
        case resume_submission::normal:
          scheduler_.schedule(handle);
          break;
        case resume_submission::io:
          scheduler_.schedule_io(handle);
          break;
        case resume_submission::yield:
          scheduler_.schedule_yield(handle);
          break;
        case resume_submission::cooperative_yield:
          if constexpr (requires { scheduler.enqueue_cooperative_yield(handle); })
            scheduler.enqueue_cooperative_yield(handle);
          else
            scheduler_.schedule_yield(handle);
          break;
      }
    }
  }

  basic_scheduler_ref<S> scheduler_;
  task_scheduling_policy policy_{};
};

using resume_target = basic_resume_target<default_scheduler>;

/** @brief 捕获当前静态调度恢复目标。 */
template <class S>
basic_resume_target<S> capture_current_resume_target(bool reserve_slot = true) {
  basic_resume_target<S> target{require_current_scheduler_for<S>(),
                                ::faio::detail::current_task_context().scheduling};
  if (reserve_slot)
    target.reserve();
  return target;
}

/** @brief 捕获当前协程恢复目标。 */
template <class S>
basic_resume_target<S> capture_resume_target(basic_scheduler_ref<S> scheduler,
                                             bool reserve_slot = true) {
  basic_resume_target<S> target{scheduler,
                                current_scheduler_matches(scheduler)
                                    ? ::faio::detail::current_task_context().scheduling
                                    : task_scheduling_policy{}};
  if (reserve_slot)
    target.reserve();
  return target;
}
}  // namespace faio::detail
