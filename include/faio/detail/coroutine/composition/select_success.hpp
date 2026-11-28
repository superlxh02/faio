/**
 * @file faio/detail/coroutine/composition/select_success.hpp
 * @brief 选出最先发布成功终态的分支，排空后交付结果；全部失败时按输入顺序汇总。
 */
#pragma once

#include "faio/detail/coroutine/composition/settle.hpp"
#include "faio/detail/coroutine/launch/root_task.hpp"
#include "faio/detail/coroutine/wait/completion_event.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace faio {
/** @brief 未取得任何成功结果时，按输入位置保存单个分支的失败或停止。 */
struct branch_failure {
  std::size_t index;                            ///< 分支在原始输入中的位置。
  task_outcome_status status;                   ///< error 或 stopped。
  std::exception_ptr exception;                 ///< 原异常，停止可无原异常。
  std::optional<task_stop_reason> stop_reason;  ///< 停止原因，错误时为空。
};

/** @brief 成功时同时保存赢家下标和值，否则保存按输入位置排列的失败和停止。 */
template <class... Ts>
struct select_success_result {
  using variant_type = std::variant<detail::joined_value<Ts>...>;

  std::optional<std::size_t> winner_index;  ///< 成功分支在原始输入中的位置。
  std::optional<variant_type> value;        ///< 用同一 variant 下标读取结果，void 用 monostate。
  std::vector<branch_failure> failures;     ///< 只有所有分支均未成功时才填充。
};

namespace detail {
/** @brief 父协程拥有各分支槽和排空状态，子分支仅在承担工作责任期间借用。 */
template <class S, class... Ts>
struct select_success_state {
  std::tuple<std::optional<task_outcome<Ts>>...> outcomes;  ///< 各分支独占构造自己的结果槽。
  std::atomic<std::size_t> winner{sizeof...(Ts)};           ///< 哨兵表示尚无成功。
  std::atomic<std::size_t> pending{1};                      ///< 启动过程额外占用一个计数。
  basic_completion_event<S> drained;                        ///< 已接纳的业务帧全部归还。
  std::shared_ptr<linked_stop_state> stop;                  ///< 保留父子停止连接。

  /** @brief 沿用现有链接停止源，父请求只向分支传播。 */
  explicit select_success_state(const task_context& parent)
      : stop(std::make_shared<linked_stop_state>(parent.stop_token, parent.cancellation_owner)) {}

  /** @brief 归还一份工作计数，计数归零时通知已排空。 */
  void finish() {
    if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
      drained.notify();
  }
};

/** @brief 原生等待协议回收业务协程帧后，对应的成功结果槽才能参与胜出竞争。 */
template <std::size_t I, class T, class S, class... Ts>
root_task select_success_root_task_wrapper(task<T, S> work, select_success_state<S, Ts...>* state) {
  auto& outcome = std::get<I>(state->outcomes);
  try {
    outcome.emplace(co_await settling_awaiter<T, S>{std::move(work).operator co_await()});
  } catch (...) {
    outcome.emplace(task_outcome<T>::from_error(std::current_exception()));
  }
  if (outcome->status() == task_outcome_status::VALUE) {
    std::size_t expected = sizeof...(Ts);
    if (state->winner.compare_exchange_strong(expected, I, std::memory_order_acq_rel))
      state->stop->source.request_stop();
    else
      outcome.reset();
  }
  state->finish();
}

/** @brief 初始化分支根并复用已有的投递与失败回滚协议。 */
template <std::size_t I, class T, class S, class... Ts>
void schedule_select_success_task(task<T, S> work, select_success_state<S, Ts...>& state) {
  const auto scheduler = require_current_scheduler_for<S>();
  auto context = capture_task_context();
  auto* scope = context.scope;
  context.stop_token = state.stop->source.get_token();
  context.cancellation_owner = state.stop;
  auto root = select_success_root_task_wrapper<I>(std::move(work), &state);
  root.set_context(std::move(context));
  state.pending.fetch_add(1, std::memory_order_relaxed);
  if (scope)
    scope->add();
  try {
    schedule_task(std::move(root), scheduler, scope);
  } catch (...) {
    state.finish();
    throw;
  }
}

/** @brief 按输入索引配对分支与结果槽。提交中断时，本函数销毁尚未接纳的参数。 */
template <class S, class... Ts, std::size_t... Is>
void schedule_select_success_tasks(select_success_state<S, Ts...>& state,
                                   std::index_sequence<Is...>,
                                   task<Ts, S>... work) {
  (schedule_select_success_task<Is>(std::move(work), state), ...);
}

/** @brief 只在已排空后将赢家值移入公开 variant。 */
template <std::size_t I, class T, class Variant>
void take_success_value(std::optional<Variant>& result, task_outcome<T>& outcome) {
  if constexpr (std::is_void_v<T>) {
    outcome.take_value();
    result.emplace(std::in_place_index<I>);
  } else {
    result.emplace(std::in_place_index<I>, outcome.take_value());
  }
}

/** @brief 排空后移出赢家结果，全部失败时按索引读取每项终态。 */
template <class S, class... Ts, std::size_t... Is>
select_success_result<Ts...> collect_success(select_success_state<S, Ts...>& state,
                                             std::index_sequence<Is...>) {
  select_success_result<Ts...> result;
  const auto index = state.winner.load(std::memory_order_acquire);
  if (index != sizeof...(Ts)) {
    result.winner_index = index;
    ((index == Is ? take_success_value<Is>(result.value, *std::get<Is>(state.outcomes)) : void()),
     ...);
  } else {
    result.failures.reserve(sizeof...(Ts));
    (result.failures.push_back(branch_failure{Is,
                                              std::get<Is>(state.outcomes)->status(),
                                              std::get<Is>(state.outcomes)->exception(),
                                              std::get<Is>(state.outcomes)->stop_reason()}),
     ...);
  }
  return result;
}
}  // namespace detail

/**
 * @brief 惰性启动候选，返回先发布成功结果的分支，或全部分支的失败与停止终态。
 * @details 只有成功构造结果槽的分支参加 CAS。分支胜出或父级请求停止时，均只发出协作停止请求，
 * 每个已接纳分支退出后才返回。胜出下标与 variant 对应原输入位置。
 * @param children 交给选择器的至少一个惰性任务。
 * @throws 提交失败时先排空已接纳的分支，再传播原异常。消费赢家值也可能抛出。
 */
template <class S = default_scheduler, class... Ts>
  requires(sizeof...(Ts) > 0)
task<select_success_result<Ts...>, S> select_success(task<Ts, S>... children) {
  auto drain_target = detail::capture_current_resume_target<S>();
  detail::select_success_state<S, Ts...> state{detail::current_task_context()};
  std::exception_ptr launch_error;
  try {
    detail::schedule_select_success_tasks(
        state, std::index_sequence_for<Ts...>{}, std::move(children)...);
  } catch (...) {
    launch_error = std::current_exception();
    state.stop->source.request_stop();
  }
  state.finish();
  co_await state.drained.wait(std::move(drain_target));
  if (launch_error)
    std::rethrow_exception(launch_error);
  co_return detail::collect_success(state, std::index_sequence_for<Ts...>{});
}
}  // namespace faio
