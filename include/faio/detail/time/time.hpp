/**
 * @file faio/detail/time/time.hpp
 * @brief 时间点、时长与异步等待。
 */
#ifndef FAIO_DETAIL_TIME_TIME_HPP
#define FAIO_DETAIL_TIME_TIME_HPP

#include "faio/detail/time/interval.hpp"
#include "faio/detail/time/sleep.hpp"
#include "faio/detail/time/timeout.hpp"
#include <chrono>
#include <utility>

namespace faio::time {
/**
 *  @brief 为 I/O 操作保存绝对截止时间，不登记定时器或提交 I/O。
 * @param operation 采用完整或紧凑策略的等待者；右值移入返回值，左值继续引用原对象。
 * @param deadline 同一 steady_clock 的绝对时间点；移动不会重启截止时间。
 * @return 传入右值时返回持有操作的值，传入左值时返回原操作的引用。
 * @details decltype(auto) 保留 setter 的值类别，避免按值返回不可复制的左值。
 *          右值 setter 返回对象值，因此这里不会返回指向临时等待者的右值引用。
 */
template <class T>
  requires io::detail::io_registrant_operation<T>
           && requires(T&& operation, std::chrono::steady_clock::time_point deadline) {
                std::forward<T>(operation).set_timeout_at(deadline);
              }
decltype(auto) timeout_at(T&& operation, std::chrono::steady_clock::time_point deadline) noexcept(
    noexcept(std::forward<T>(operation).set_timeout_at(deadline))) {
  return std::forward<T>(operation).set_timeout_at(deadline);
}

/**
 *  @brief 为 I/O 操作设置相对超时，右值转移所有权，左值保持借用。
 * @param operation 采用完整或紧凑策略的等待者；不会复制不可复制的等待者。
 * @param interval 从调用时起计算的毫秒时长，不会推迟至 await 时重新计时。
 * @return 传入右值时返回持有操作的值，传入左值时返回原操作的引用。
 */
template <class T>
  requires io::detail::io_registrant_operation<T>
           && requires(T&& operation, std::chrono::milliseconds interval) {
                std::forward<T>(operation).set_timeout(interval);
              }
decltype(auto) timeout(T&& operation, std::chrono::milliseconds interval) noexcept(
    noexcept(std::forward<T>(operation).set_timeout(interval))) {
  return std::forward<T>(operation).set_timeout(interval);
}

/**
 * @brief 让当前协程等待指定时长。
 * duration <= 0 时立即返回，无需挂起。
 */
static inline auto sleep(const std::chrono::nanoseconds& duration) {
  auto now = std::chrono::steady_clock::now();
  if (duration.count() <= 0) {
    return detail::Sleep{now};
  }
  return detail::Sleep{now + duration};
}

/** @brief 让当前协程等待到指定的绝对时间点。 */
static inline auto sleep_until(std::chrono::steady_clock::time_point expired_time) {
  return detail::Sleep{expired_time};
}

/** @brief 创建周期定时器，首次在一个 period 后触发。 */
static inline auto interval(std::chrono::nanoseconds period) {
  return detail::Interval{std::chrono::steady_clock::now(), period};
}

/** @brief 创建周期定时器，首次在 start + period 时触发。 */
static inline auto interval_at(std::chrono::steady_clock::time_point start,
                               std::chrono::nanoseconds period) {
  return detail::Interval{start, period};
}
}  // namespace faio::time
#endif
