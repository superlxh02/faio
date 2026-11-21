/**
 * @file faio/detail/coroutine/composition/complete_before.hpp
 * @brief 通过惰性触发任务决定是否截止，并等待工作任务和触发任务全部退出。
 */
#pragma once

#include "faio/detail/coroutine/composition/task_group.hpp"
#include "faio/detail/coroutine/composition/with_stop_token.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <variant>

namespace faio {
/** @brief 截止组合的互斥终态，分别记录工作和触发器的错误。 */
enum class deadline_status { value, work_error, trigger_error, deadline, stopped };

namespace detail {
template <class T>
struct deadline_race;
}

/** @brief 拥有截止组合的唯一结果消费权。 */
template <class T>
class [[nodiscard]] deadline_outcome {
 public:
  deadline_outcome(deadline_outcome&&) noexcept = default;

  deadline_outcome(const deadline_outcome&) = delete;
  deadline_outcome& operator=(const deadline_outcome&) = delete;
  deadline_outcome& operator=(deadline_outcome&&) = delete;

  /** @brief 根据胜出分支及其实际终态查询结果，领取值后仍可查询。 */
  deadline_status status() const noexcept {
    if (const auto* work = std::get_if<0>(&winner_)) {
      if (work->status() == task_outcome_status::value)
        return deadline_status::value;
      if (work->status() == task_outcome_status::error)
        return deadline_status::work_error;
    } else {
      const auto& trigger = std::get<1>(winner_);
      if (trigger.status() == task_outcome_status::value)
        return deadline_status::deadline;
      if (trigger.status() == task_outcome_status::error)
        return deadline_status::trigger_error;
    }
    return deadline_status::stopped;
  }

  /** @brief 返回错误或停止关联的异常，工作成功或截止时为空。 */
  std::exception_ptr exception() const noexcept {
    return std::visit([](const auto& outcome) { return outcome.exception(); }, winner_);
  }

  /** @brief 领取一次成功的工作结果。 */
  T take_value() {
    auto* work = std::get_if<0>(&winner_);
    if (!work)
      throw std::logic_error("截止组合没有可领取的工作结果");
    return work->take_value();
  }

 private:
  friend struct detail::deadline_race<T>;

  template <std::size_t I, class U>
  explicit deadline_outcome(std::in_place_index_t<I> index, task_outcome<U>&& outcome) noexcept
      : winner_(index, std::move(outcome)) {}

  std::variant<task_outcome<T>, task_outcome<void>> winner_;
};

namespace detail {
enum class deadline_branch_id { none, work, trigger };

/** @brief 两条分支使用独立输出槽，排空后由调用方领取胜出结果。 */
template <class T>
struct deadline_race {
  std::stop_source stop;
  std::atomic<deadline_branch_id> winner{deadline_branch_id::none};
  std::optional<task_outcome<T>> work;
  std::optional<task_outcome<void>> trigger;

  /** @brief 终态构造完成后竞争胜出权，仅胜出分支请求同组停止。 */
  void publish(deadline_branch_id branch) noexcept {
    auto expected = deadline_branch_id::none;
    if (winner.compare_exchange_strong(expected, branch, std::memory_order_acq_rel))
      stop.request_stop();
  }

  /** @brief 只在两个分支均已退出后移动胜出槽。 */
  deadline_outcome<T> take_winner() {
    switch (winner.load(std::memory_order_acquire)) {
      case deadline_branch_id::work:
        return deadline_outcome<T>{std::in_place_index<0>, std::move(*work)};
      case deadline_branch_id::trigger:
        return deadline_outcome<T>{std::in_place_index<1>, std::move(*trigger)};
      case deadline_branch_id::none:
        throw std::logic_error("截止组合结束时没有已发布的分支终态");
    }
    std::terminate();
  }
};

/** @brief 观察并发布任务的实际终态；连接停止源或取得值时发生的失败记为当前分支的错误。 */
template <class U, class S, class T>
task<void, S> run_deadline_branch(task<U, S> work,
                                  std::optional<task_outcome<U>>& outcome,
                                  deadline_race<T>& race,
                                  deadline_branch_id branch) {
  try {
    outcome.emplace(co_await with_stop_token_awaiter<U, S, true>{
        {std::move(work).operator co_await()}, race.stop.get_token()});
  } catch (...) {
    outcome.emplace(task_outcome<U>::from_error(std::current_exception()));
  }
  race.publish(branch);
}
}  // namespace detail

/**
 * @brief 等待工作或触发任务的首个终态，并在排空双方后交付结果。
 * @details 触发任务先成功完成时返回 deadline，先抛出异常时返回 trigger_error。
 * 任一分支先以协作停止结束时返回 stopped。父级停止请求转发给双方，最先实际发布的终态决定结果。
 * 分支胜出后向双方请求停止；其余分支仍须完成自身清理，双方全部退出后才向调用者交付结果。
 * @param work 需要等待并判断是否截止的工作任务。
 * @param trigger 成功完成时触发截止的惰性任务。
 * @details 建立任务组或接纳分支失败时，排空已接纳工作后传播原异常。
 */
template <class T, class S>
task<deadline_outcome<T>, S> complete_before(task<T, S> work, task<void, S> trigger) {
  detail::deadline_race<T> race;
  auto summary = co_await task_group<S>(
      group_options{group_error_policy::collect_all},
      [&race, work = std::move(work), trigger = std::move(trigger)](
          basic_task_group_context<S>& group) mutable -> task<void, S> {
        try {
          (void)group.spawn_observed(detail::run_deadline_branch(
              std::move(work), race.work, race, detail::deadline_branch_id::work));
          (void)group.spawn_observed(detail::run_deadline_branch(
              std::move(trigger), race.trigger, race, detail::deadline_branch_id::trigger));
        } catch (...) {
          race.stop.request_stop();
          throw;
        }
        co_return;
      });
  summary.body.value_or_throw();
  co_return race.take_winner();
}
}  // namespace faio
