/**
 * @file faio/detail/coroutine/composition/result.hpp
 * @brief 协程并发组合的结果类型，保存各任务的状态。
 */
#pragma once

#include "faio/detail/common/cancellation.hpp"
#include <concepts>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace faio::detail {
/** @brief 统一结果存储类型：void 用 monostate 表示正常完成，其余类型保存用户值。 */
template <class T>
using handle_value = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

/** @brief 并发组合的 void 分支以 monostate 保留参数位置。 */
template <class T>
using joined_value = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
}  // namespace faio::detail

namespace faio {
/** @brief 工作发布的互斥终态，void 正常完成也使用 value。 */
enum class task_outcome_status { value, error, stopped };

/** @brief 区分已运行任务的停止和因停止而从未接纳的任务。 */
enum class task_stop_reason { requested, not_started };

/**
 * @brief 拥有任务的终态及单次值消费权。
 * @details 即使领取结果时移动 T 抛出异常，终态仍保持 value，消费权也视为已使用。
 * 值可无异常移动时直接保存。否则构造结果时单独分配 T，使发布后的结果转交不再移动 T。
 */
template <class T>
class [[nodiscard]] task_outcome {
  static_assert(std::is_void_v<T>
                    || (std::is_object_v<T> && !std::is_array_v<T>
                        && std::is_move_constructible_v<T>),
                "task_outcome<T> requires void or a move-constructible value type");

  using value_type = detail::handle_value<T>;
  static constexpr bool stores_indirectly = !std::is_nothrow_move_constructible_v<value_type>;

  /** @brief 独立保存移动时可能抛出异常的值，转交结果时只移动其所有者。 */
  struct owned_value {
    template <class U>
    explicit owned_value(U&& source) : value(std::forward<U>(source)) {}

    value_type value;
  };

  using stored_value =
      std::conditional_t<stores_indirectly, std::unique_ptr<owned_value>, value_type>;

  struct value_slot {
    template <class... Args>
      requires(!stores_indirectly)
    explicit value_slot(std::in_place_t, Args&&... args)
        : payload(std::in_place, std::forward<Args>(args)...) {}

    template <class U>
      requires stores_indirectly
    explicit value_slot(std::in_place_t, U&& value)
        : payload(std::in_place, std::make_unique<owned_value>(std::forward<U>(value))) {}

    value_slot(value_slot&& source) noexcept : consumed(std::exchange(source.consumed, true)) {
      if (!consumed)
        payload.emplace(std::move(*source.payload));
    }

    std::optional<stored_value> payload;
    bool consumed{};
  };

  struct failure_slot {
    failure_slot(task_outcome_status terminal,
                 task_stop_reason stop,
                 std::exception_ptr cause) noexcept
        : status(terminal), reason(stop), exception(std::move(cause)) {}

    /** @brief 复制异常引用，使源结果在移动后仍可读取已发布的终态元数据。 */
    failure_slot(const failure_slot&) noexcept = default;

    task_outcome_status status;
    task_stop_reason reason;
    std::exception_ptr exception;
  };

 public:
  /** @brief 构造成功结果，在发布前完成值的构造和可能的分配。 */
  template <class U>
    requires(!std::is_void_v<T> && std::constructible_from<T, U &&>)
  static task_outcome from_value(U&& value) {
    return task_outcome(std::in_place, std::forward<U>(value));
  }

  /** @brief 构造无值任务的正常终态，不分配存储。 */
  static task_outcome from_value() noexcept
    requires std::is_void_v<T>
  {
    return task_outcome(std::in_place);
  }

  /** @brief 保存已发布的失败异常。 */
  static task_outcome from_error(std::exception_ptr exception) {
    if (!exception)
      throw std::invalid_argument("task_outcome 的 error 终态需要异常");
    return task_outcome(
        task_outcome_status::error, task_stop_reason::requested, std::move(exception));
  }

  /** @brief 保存停止终态及可选的原异常。 */
  static task_outcome from_stopped(std::exception_ptr exception = {}) noexcept {
    return from_stopped(task_stop_reason::requested, std::move(exception));
  }

  /** @brief 保存显式停止原因及可选的原异常。 */
  template <class Reason>
    requires std::same_as<Reason, task_stop_reason>
  static task_outcome from_stopped(Reason reason, std::exception_ptr exception = {}) noexcept {
    return task_outcome(task_outcome_status::stopped, reason, std::move(exception));
  }

  /** @brief 转交结果及其唯一消费权，保留源结果的终态。 */
  task_outcome(task_outcome&&) noexcept = default;

  task_outcome(const task_outcome&) = delete;
  task_outcome& operator=(const task_outcome&) = delete;
  task_outcome& operator=(task_outcome&&) = delete;

  /** @brief 查询已发布的终态，领取值后仍可查询。 */
  task_outcome_status status() const noexcept {
    if (const auto* failure = std::get_if<failure_slot>(&storage_))
      return failure->status;
    return task_outcome_status::value;
  }

  /** @brief 查询停止原因，成功和失败终态返回空。 */
  std::optional<task_stop_reason> stop_reason() const noexcept {
    const auto* failure = std::get_if<failure_slot>(&storage_);
    if (failure && failure->status == task_outcome_status::stopped)
      return failure->reason;
    return std::nullopt;
  }

  /** @brief 查询失败异常或停止附带的异常，成功和无原异常的停止返回空。 */
  std::exception_ptr exception() const noexcept {
    if (const auto* failure = std::get_if<failure_slot>(&storage_))
      return failure->exception;
    return {};
  }

  /** @brief 领取一次成功值；非成功终态或消费权已用完时抛出逻辑错误。 */
  T take_value() {
    auto* slot = std::get_if<value_slot>(&storage_);
    if (!slot || std::exchange(slot->consumed, true))
      throw std::logic_error("task_outcome 没有可领取的成功值");
    if constexpr (!std::is_void_v<T>) {
      if constexpr (stores_indirectly)
        return std::move((*slot->payload)->value);
      else
        return std::move(*slot->payload);
    }
  }

  /** @brief 领取成功值或重抛原异常；停止没有原异常时抛出 operation_cancelled。 */
  T value_or_throw() {
    if (status() == task_outcome_status::value)
      return take_value();
    if (auto cause = exception())
      std::rethrow_exception(std::move(cause));
    throw operation_cancelled{};
  }

 private:
  template <class... Args>
  explicit task_outcome(std::in_place_t, Args&&... args)
      : storage_(std::in_place_type<value_slot>, std::in_place, std::forward<Args>(args)...) {}

  task_outcome(task_outcome_status status,
               task_stop_reason reason,
               std::exception_ptr exception) noexcept
      : storage_(std::in_place_type<failure_slot>, status, reason, std::move(exception)) {}

  std::variant<value_slot, failure_slot> storage_;
};
}  // namespace faio
