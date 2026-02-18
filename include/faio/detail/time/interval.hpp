#ifndef FAIO_DETAIL_TIME_INTERVAL_HPP
#define FAIO_DETAIL_TIME_INTERVAL_HPP

#include "faio/detail/time/sleep.hpp"
#include <chrono>

namespace faio::time {
// 错过 tick 的处理方式
enum class MissedTickBehavior {
  /// 尽快补发所有错过的 tick（连续触发直到追上）
  Burst,
  /// 从当前时间开始延迟一个 period
  Delay,
  /// 跳过错过的 tick，对齐到下一个自然周期点
  Skip,
};
} // namespace faio::time

namespace faio::time::detail {
class Interval {
public:
  Interval(std::chrono::steady_clock::time_point first_expired_time,
           std::chrono::nanoseconds period,
           MissedTickBehavior behavior = MissedTickBehavior::Burst)
      : _deadline{first_expired_time + period}, _period{period},
        _behavior{behavior} {}

  /// 等待下一个 tick 到来
  /// 返回一个 Sleep awaiter，co_await 之后在 deadline 到期时恢复
  auto tick() noexcept -> Sleep {
    auto expired_time = _deadline;
    _deadline = next_timeout();
    return Sleep{expired_time};
  }

  /// 获取当前周期
  [[nodiscard]]
  auto period() const noexcept -> std::chrono::nanoseconds {
    return _period;
  }

  /// 重置定时器，下一个 tick 在一个 period 之后
  void reset() noexcept {
    _deadline = std::chrono::steady_clock::now() + _period;
  }

  /// 重置定时器，下一个 tick 立即触发
  void reset_immediately() noexcept {
    _deadline = std::chrono::steady_clock::now();
  }

  /// 重置定时器，下一个 tick 在指定时间后触发
  void reset_after(std::chrono::nanoseconds after) noexcept {
    _deadline = std::chrono::steady_clock::now() + after;
  }

  /// 重置定时器到指定的绝对时间点
  void reset_at(std::chrono::steady_clock::time_point deadline) noexcept {
    _deadline = deadline;
  }

  [[nodiscard]]
  auto missed_tick_behavior() const noexcept -> MissedTickBehavior {
    return _behavior;
  }

  void set_missed_tick_behavior(MissedTickBehavior behavior) noexcept {
    _behavior = behavior;
  }

private:
  auto next_timeout() -> std::chrono::steady_clock::time_point {
    auto now = std::chrono::steady_clock::now();
    switch (_behavior) {
    case MissedTickBehavior::Burst:
      return _deadline + _period;
    case MissedTickBehavior::Delay:
      return now + _period;
    case MissedTickBehavior::Skip: {
      // 如果尚未错过，直接返回下一个周期
      if (_deadline >= now) {
        return _deadline + _period;
      }
      // 否则对齐到下一个自然周期点
      auto missed_ns = (now - _deadline).count();
      auto period_ns = _period.count();
      auto skip = missed_ns / period_ns + 1;
      return _deadline + std::chrono::nanoseconds{skip * period_ns};
    }
    }
    std::unreachable();
  }

private:
  std::chrono::steady_clock::time_point _deadline;
  std::chrono::nanoseconds _period;
  MissedTickBehavior _behavior;
};
} // namespace faio::time::detail

#endif // FAIO_DETAIL_TIME_INTERVAL_HPP