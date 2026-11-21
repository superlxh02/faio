/**
 * @file faio/detail/coroutine/launch/observed_task.hpp
 * @brief 用户 task 与可观察根任务的调度及结果发布桥接。
 */
#pragma once

#include "faio/detail/coroutine/composition/join_state.hpp"
#include "faio/detail/coroutine/launch/root_task.hpp"
#include <exception>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace faio::detail {
/**
 * @brief 包装用户 task 的根协程，捕获结果或异常并发布到共享状态。
 * state 的 shared_ptr 使句柄先析构时状态仍存活，tracker 仅借用外层任务组。
 */
template <class T, class S>
root_task observed_root_task_wrapper(task<T, S> child,
                                     std::shared_ptr<join_handle_state<T, S>> state) {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(child);
      state->value.emplace();
    } else {
      state->value.emplace(co_await std::move(child));
    }
  } catch (...) {
    state->capture_error(std::current_exception());
  }
  state->publish();
}

/**
 * @brief 调度可观察结果的任务，返回供 JoinHandle 持有的共享状态。
 * 父令牌只转发停止请求；没有 tracker 时任务仍登记到运行时活动根任务计数。
 */
template <class T, class S>
std::shared_ptr<join_handle_state<T, S>> schedule_observed_task(
    scheduler_ref_for<S> scheduler,
    task<T, S> child,
    task_tracker* tracker,
    const std::stop_token& parent_stop,
    task_lifetime_ref lifetime = current_task_lifetime(),
    std::shared_ptr<cancellation_state> parent_owner = current_task_context().cancellation_owner) {
  if (!scheduler)
    throw std::logic_error("没有可用的运行时");
  auto state = std::make_shared<join_handle_state<T, S>>();
  const bool matching_stop = parent_stop == current_task_context().stop_token
                             || (parent_owner && parent_stop == parent_owner->source.get_token());
  const bool inherit =
      !has_current_scheduler() || (current_scheduler_matches(scheduler) && matching_stop);
  if (!inherit) {
    state->cancellation_policy = cancellation_error_policy::fatal_if_unobserved;
    parent_owner.reset();
  }
  if (parent_owner && parent_stop != parent_owner->source.get_token()
      && parent_stop != current_task_context().stop_token)
    parent_owner.reset();
  if (state->cancellation_policy == cancellation_error_policy::normal_if_stop_requested
      || parent_owner) {
    state->cancellation_owner =
        std::make_shared<cancellation_state>(parent_stop, std::move(parent_owner));
    state->stop_source = state->cancellation_owner->source;
  }
  if (!state->cancellation_owner && parent_stop.stop_possible())
    state->parent_callback.emplace(parent_stop, forward_stop{&state->stop_source});
  auto context = inherit ? capture_task_context() : task_context{};
  context.scope = tracker;
  context.stop_token = state->stop_source.get_token();
  context.cancellation_policy = state->cancellation_policy;
  context.cancellation_owner = state->cancellation_owner;
  auto root = observed_root_task_wrapper(std::move(child), state);
  root.set_context(std::move(context));
  if (tracker)
    tracker->add();
  schedule_task(std::move(root), scheduler, tracker, lifetime);
  return state;
}
}  // namespace faio::detail
