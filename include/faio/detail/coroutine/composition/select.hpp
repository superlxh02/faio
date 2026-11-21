/**
 * @file faio/detail/coroutine/composition/select.hpp
 * @brief 首个完成分支的仲裁、协作停止与排空实现。
 */
#pragma once

#include "faio/detail/coroutine/composition/result.hpp"
#include "faio/detail/coroutine/wait/completion_event.hpp"
#include "faio/detail/coroutine/launch/root_task.hpp"
#include "faio/detail/coroutine/context/this_coro.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <optional>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace faio {
namespace detail {
/** @brief 选择状态位于父 select 帧；返回与异常传播之前排空所有已提交分支。 */
template <class S, class... Ts>
struct select_state {
  using variant_type = std::variant<
      joined_value<Ts>...>; /**< 异构结果载体，使用 variant 下标区分相同结果类型的不同分支。 */
  std::atomic<bool> claimed{
      false}; /**< 胜出权仲裁位，只允许一个分支 CAS 成功；不是结果已经发布的标志。 */
  std::optional<variant_type> value; /**< 胜出结果，只有 CAS 获胜者写入，其他分支不覆盖。 */
  std::exception_ptr error; /**< 胜出分支的异常，选择器排空后重抛；失败分支也能取得胜出权。 */
  std::size_t index{};      /**< 胜出分支在输入参数中的下标，仅 CAS 获胜者写入。 */
  std::atomic<std::size_t> running{
      1}; /**< 未退出分支数加一个启动哨兵，保护尚在提交其余子任务的阶段；中途失败时，父协程仍会等待所有已启动分支退出。 */
  ::faio::detail::basic_completion_event<S>
      all_done;          /**< 所有已启动分支退出后触发的事件，保证停止后先排空再返回。 */
  std::stop_source stop; /**< 候选任务共用停止源，赢家或外层停止回调向它发请求。 */

  /** @brief 归还分支或启动哨兵占用的计数，计数归零时唤醒排空等待者。 */
  void child_done() {
    if (running.fetch_sub(1, std::memory_order_acq_rel) == 1)
      all_done.notify();
  }
};

/** @brief 第 I 个选择分支的根协程包装器：先执行子任务，再以完成结果竞争唯一胜出权。 */
template <std::size_t I, class T, class S, class State>
root_task select_root_task_wrapper(task<T, S> child, State* state) {
  using variant_type = typename State::variant_type;
  std::optional<variant_type> value;
  std::exception_ptr error;
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(child);
      value.emplace(std::in_place_index<I>);
    } else {
      value.emplace(std::in_place_index<I>, co_await std::move(child));
    }
  } catch (...) {
    error = std::current_exception();
  }
  bool expected = false;
  if (state->claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    state->index = I;
    state->error = std::move(error);
    if (!state->error) {
      try {
        state->value.emplace(std::move(*value));
      } catch (...) {
        state->error = std::current_exception();
      }
    }
    state->stop.request_stop();
  }
  value.reset();
  error = {};
  state->child_done();
}

/** @brief 构造并立即提交第 I 个候选分支，只借用父帧状态；父任务排空后才能退出。 */
template <std::size_t I, class T, class S, class State>
void schedule_select_task(task<T, S> child, State* state) {
  auto context = capture_task_context();
  auto* scope = context.scope;
  context.stop_token = state->stop.get_token();
  auto root = select_root_task_wrapper<I>(std::move(child), state);
  root.set_context(std::move(context));
  state->running.fetch_add(1, std::memory_order_relaxed);
  if (scope)
    scope->add();
  try {
    schedule_task(std::move(root), ::faio::detail::current_scheduler_for<S>(), scope);
  } catch (...) {
    state->child_done();
    throw;
  }
}

/** @brief 按轮转起点提交异构候选，保持结果下标仍对应原始参数位置。 */
template <class S, class... Ts, std::size_t... Is>
void schedule_select_tasks(select_state<S, Ts...>* state,
                           std::index_sequence<Is...>,
                           task<Ts, S>... children) {
  auto tasks = std::tuple<task<Ts, S>...>{std::move(children)...};
  static thread_local std::size_t next_start{};
  const auto start = next_start++ % sizeof...(Ts);
  for (std::size_t step = 0; step < sizeof...(Ts); ++step) {
    const auto index = (start + step) % sizeof...(Ts);
    ((index == Is ? (schedule_select_task<Is>(std::move(std::get<Is>(tasks)), state), void())
                  : void()),
     ...);
  }
}

/** @brief 外层停止源到本次选择停止源的回调连接。 */
struct forward_select_stop {
  std::stop_source* source; /**< 借用 select_state 的停止源，回调注销前共享状态必须仍存活。 */

  /** @brief 外层停止时请求全部候选停止，父选择器仍会等待它们排空。 */
  void operator()() const noexcept { source->request_stop(); }
};
}  // namespace detail

/** @brief 选择器返回的结果，只含赢家的下标与值，不含落选任务的句柄。 */
template <class... Ts>
struct select_result {
  std::size_t index; /**< 赢家对应原输入参数位置；读取 value 时应使用同一 variant 下标。 */
  std::variant<detail::joined_value<Ts>...>
      value; /**< 赢家结果，void 以 monostate 占位；相同类型分支仍由下标区分。 */
};

/**
 * @brief 惰性启动候选任务，等待最先完成的分支，再请求停止其余分支。
 * 全部分支退出后才返回，保证外部引用和等待节点不超出 select 调用者的生命周期。
 * 最先完成的分支也可能抛出异常；该异常在排空后向调用者重抛。
 */
namespace detail {
/** @brief 等待分支胜出并排空其余分支。 */
template <class S, class... Ts>
  requires(sizeof...(Ts) > 0)
task<select_result<Ts...>, S> select_impl(task<Ts, S>... children) {
  auto drain_target = ::faio::detail::capture_current_resume_target<S>();
  detail::select_state<S, Ts...> state_storage;
  auto* state = &state_storage;
  auto parent_stop = co_await this_coro::stop_token();
  std::optional<std::stop_callback<detail::forward_select_stop>> parent_callback;
  if (parent_stop.stop_possible())
    parent_callback.emplace(parent_stop, detail::forward_select_stop{&state->stop});
  std::exception_ptr launch_error;
  try {
    detail::schedule_select_tasks(state, std::index_sequence_for<Ts...>{}, std::move(children)...);
  } catch (...) {
    launch_error = std::current_exception();
  }
  state->child_done();
  if (launch_error) {
    state->stop.request_stop();
    co_await state->all_done.wait(std::move(drain_target));
    std::rethrow_exception(launch_error);
  }
  co_await state->all_done.wait(std::move(drain_target));
  if (state->error)
    std::rethrow_exception(state->error);
  co_return select_result<Ts...>{state->index, std::move(*state->value)};
}
}  // namespace detail

/** @brief 静态竞争选择保留所有候选共用的具体调度类型。 */
template <class S = default_scheduler, class... Ts>
  requires(sizeof...(Ts) > 0)
task<select_result<Ts...>, S> select(task<Ts, S>... children) {
  return detail::select_impl<S>(std::move(children)...);
}
}  // namespace faio
