#ifndef FAIO_DETAIL_TIME_TIME_HPP
#define FAIO_DETAIL_TIME_TIME_HPP

#include "faio/detail/time/interval.hpp"
#include "faio/detail/time/sleep.hpp"
#include "faio/detail/time/timeout.hpp"
#include <chrono>

namespace faio::time {

/// 为 IO 操作设置绝对时间点超时
template <class T>
  requires std::derived_from<T, io::detail::IORegistrantAwaiter<T>>
auto timeout_at(T &&io, std::chrono::steady_clock::time_point deadline) {
  return io.set_timeout_at(deadline);
}

/// 为 IO 操作设置相对时间超时
template <class T>
  requires std::derived_from<T, io::detail::IORegistrantAwaiter<T>>
auto timeout(T &&io, std::chrono::milliseconds interval) {
  return io.set_timeout(interval);
}

/// 挂起当前协程指定时长
/// 如果 duration <= 0 则立即返回（不挂起）
static inline auto sleep(const std::chrono::nanoseconds &duration) {
  auto now = std::chrono::steady_clock::now();
  if (duration.count() <= 0) {
    return detail::Sleep{now};
  }
  return detail::Sleep{now + duration};
}

/// 挂起当前协程直到指定的绝对时间点
static inline auto
sleep_until(std::chrono::steady_clock::time_point expired_time) {
  return detail::Sleep{expired_time};
}

/// 创建一个周期性定时器，首次 tick 在一个 period 之后触发
static inline auto interval(std::chrono::nanoseconds period) {
  return detail::Interval{std::chrono::steady_clock::now(), period};
}

/// 创建一个周期性定时器，首次 tick 在 start + period 触发
static inline auto interval_at(std::chrono::steady_clock::time_point start,
                               std::chrono::nanoseconds period) {
  return detail::Interval{start, period};
}

} // namespace faio::time

#endif // FAIO_DETAIL_TIME_TIME_HPP