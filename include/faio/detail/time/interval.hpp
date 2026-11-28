/**
 * @file faio/detail/time/interval.hpp
 * @brief 周期定时器。
 */
#ifndef FAIO_DETAIL_TIME_INTERVAL_HPP
#define FAIO_DETAIL_TIME_INTERVAL_HPP

#include "faio/detail/time/sleep.hpp"
#include <chrono>

namespace faio::time {
/** @brief 错过定时触发时的处理方式。 */
enum class missed_tick_behavior {
  BURST,
  DELAY,
  SKIP,
};
}  // namespace faio::time

namespace faio::time::detail {
/** @brief 周期定时器。 */
class interval_timer {
 public:
  /** @brief 构造周期定时器。 */
  interval_timer(std::chrono::steady_clock::time_point first_expired_time,
                 std::chrono::nanoseconds period,
                 missed_tick_behavior behavior = missed_tick_behavior::BURST)
      : _deadline{first_expired_time + period}, _period{period}, _behavior{behavior} {}

  /**
   * @brief 等待下一次定时触发。
   * 返回 sleep_awaiter 等待器，co_await 后在 deadline 到期时恢复。
   */
  auto tick() noexcept -> sleep_awaiter {
    auto expired_time = _deadline;
    _deadline = next_timeout();
    return sleep_awaiter{expired_time};
  }

  /** @brief 取得当前周期。 */
  [[nodiscard]]
  auto period() const noexcept -> std::chrono::nanoseconds {
    return _period;
  }

  /** @brief 重置定时器，在一个 period 后再次触发。 */
  void reset() noexcept { _deadline = std::chrono::steady_clock::now() + _period; }

  /** @brief 重置定时器，使下一次定时立即触发。 */
  void reset_immediately() noexcept { _deadline = std::chrono::steady_clock::now(); }

  /** @brief 重置定时器，在指定时长后再次触发。 */
  void reset_after(std::chrono::nanoseconds after) noexcept {
    _deadline = std::chrono::steady_clock::now() + after;
  }

  /** @brief 将定时器重置到指定的绝对时间点。 */
  void reset_at(std::chrono::steady_clock::time_point deadline) noexcept { _deadline = deadline; }

  /** @brief 读取错过周期的处理策略。 */
  [[nodiscard]]
  auto missed_tick_behavior() const noexcept -> ::faio::time::missed_tick_behavior {
    return _behavior;
  }

  /** @brief 设置错过周期的处理策略。 */
  void set_missed_tick_behavior(::faio::time::missed_tick_behavior behavior) noexcept {
    _behavior = behavior;
  }

 private:
  /** @brief 计算下一次触发的时间点。 */
  auto next_timeout() -> std::chrono::steady_clock::time_point {
    auto now = std::chrono::steady_clock::now();
    switch (_behavior) {
      case missed_tick_behavior::BURST:
        return _deadline + _period;
      case missed_tick_behavior::DELAY:
        return now + _period;
      case missed_tick_behavior::SKIP: {
        if (_deadline >= now) {
          return _deadline + _period;
        }
        auto missed_ns = (now - _deadline).count();
        auto period_ns = _period.count();
        auto skip = missed_ns / period_ns + 1;
        return _deadline + std::chrono::nanoseconds{skip * period_ns};
      }
    }
    std::unreachable();
  }

 private:
  std::chrono::steady_clock::time_point _deadline; /**< 下一次触发的单调时钟绝对时间点。 */
  std::chrono::nanoseconds _period;                /**< 周期定时器的固定触发间隔。 */
  ::faio::time::missed_tick_behavior _behavior;    /**< 错过触发时采用的策略：追赶、延迟或跳过。 */
};
}  // namespace faio::time::detail
#endif
