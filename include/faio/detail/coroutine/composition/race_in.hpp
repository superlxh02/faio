/**
 * @file faio/detail/coroutine/composition/race_in.hpp
 * @brief 返回最先发布的终态，由词法任务组负责排空。
 */
#pragma once

#include "faio/detail/coroutine/composition/task_group.hpp"
#include <array>
#include <tuple>
#include <variant>

namespace faio {
namespace detail {
/** @brief 每个分支只写自己的槽，发布后由结果持有者读取。 */
struct race_loser_error {
  std::exception_ptr error;
  std::atomic<bool> ready{};
};
}  // namespace detail

/** @brief 只持有胜出分支的终态；组结束后可通过诊断对象查看其他分支后来发布的错误。 */
template <class... Ts>
struct race_result {
  std::size_t index;
  std::variant<task_outcome<Ts>...> outcome;
  std::shared_ptr<const std::array<detail::race_loser_error, sizeof...(Ts)>> diagnostics;

  /** @brief 查询其他分支后来发布的错误；竞速完成时，未胜出的分支可能仍在执行。 */
  std::exception_ptr loser_error(std::size_t branch) const {
    const auto& slot = diagnostics->at(branch);
    return slot.ready.load(std::memory_order_acquire) ? slot.error : std::exception_ptr{};
  }
};

namespace detail {
template <class S, class... Ts>
struct race_state {
  using error_array = std::array<race_loser_error, sizeof...(Ts)>;
  std::tuple<std::optional<task_outcome<Ts>>...> outcomes;
  std::atomic<std::size_t> winner{sizeof...(Ts)};
  error_array errors{};
  basic_completion_event<S> ready;
  std::shared_ptr<linked_stop_state> stop;

  explicit race_state(const std::shared_ptr<linked_stop_state>& parent)
      : stop(std::make_shared<linked_stop_state>(parent->source.get_token(), parent)) {}
};

template <std::size_t I, class T, class S, class... Ts>
root_task race_root_task_wrapper(task<T, S> work,
                                 basic_task_group_state<S>* owner,
                                 std::shared_ptr<race_state<S, Ts...>> state) {
  auto& outcome = std::get<I>(state->outcomes);
  try {
    outcome.emplace(co_await settling_awaiter<T, S>{std::move(work).operator co_await()});
  } catch (...) {
    outcome.emplace(task_outcome<T>::from_error(std::current_exception()));
  }
  std::size_t expected = sizeof...(Ts);
  if (state->winner.compare_exchange_strong(expected, I, std::memory_order_acq_rel)) {
    state->stop->source.request_stop();
    state->ready.notify();
  } else {
    if (outcome->status() == task_outcome_status::error) {
      auto& slot = state->errors[I];
      slot.error = outcome->exception();
      slot.ready.store(true, std::memory_order_release);
    }
    outcome.reset();
  }
  owner->finish();
}

template <std::size_t I = 0, class S, class... Ts>
race_result<Ts...> take_race(const std::shared_ptr<race_state<S, Ts...>>& state,
                             std::size_t index) {
  if constexpr (I < sizeof...(Ts)) {
    if (index == I)
      return {I,
              std::variant<task_outcome<Ts>...>{std::in_place_index<I>,
                                                std::move(*std::get<I>(state->outcomes))},
              {state, &state->errors}};
    return take_race<I + 1>(state, index);
  } else {
    std::terminate();
  }
}

template <class S, class... Ts, std::size_t... Is>
void schedule_race_tasks(basic_task_group_context<S>& group,
                         const std::shared_ptr<race_state<S, Ts...>>& state,
                         std::index_sequence<Is...>,
                         task<Ts, S>... work) {
  auto* owner = &group_lifetime_access::state(group);
  (group_lifetime_access::schedule_task(
       group, race_root_task_wrapper<Is>(std::move(work), owner, state), state->stop),
   ...);
}
}  // namespace detail

/**
 * @brief 等待最先发布的终态，再向竞速分支请求停止，由词法任务组负责排空。
 * @details 分支借用的调用方资源必须在整个组退出前保持有效；竞速必须在组主体完成前启动。
 */
template <class S, class... Ts>
  requires(sizeof...(Ts) != 0)
task<race_result<Ts...>, S> race_in(basic_task_group_context<S>& group, task<Ts, S>... children) {
  auto target = detail::capture_current_resume_target<S>();
  auto state = std::make_shared<detail::race_state<S, Ts...>>(
      detail::group_lifetime_access::state(group).stop_owner);
  try {
    detail::schedule_race_tasks(
        group, state, std::index_sequence_for<Ts...>{}, std::move(children)...);
  } catch (...) {
    state->stop->source.request_stop();
    throw;
  }
  co_await state->ready.wait(std::move(target));
  co_return detail::take_race(state, state->winner.load(std::memory_order_acquire));
}
}  // namespace faio
