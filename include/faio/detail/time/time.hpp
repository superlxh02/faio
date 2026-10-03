#ifndef FAIO_DETAIL_TIME_TIME_HPP
#define FAIO_DETAIL_TIME_TIME_HPP

#include "faio/detail/time/interval.hpp"
#include "faio/detail/time/sleep.hpp"
#include "faio/detail/time/timeout.hpp"
#include <chrono>
#include <utility>

namespace faio::time {

/** @brief 为 IO 操作保存绝对 deadline，不注册 timer 或提交 IO。
 * @param operation 完整或紧凑策略的等待者；右值移入返回值，左值保留对象身份。
 * @param deadline 同一 steady_clock 的绝对时间点；移动不会重启截止时间。
 * @return 右值输入返回拥有型操作值，左值输入返回原操作引用。
 * @details decltype(auto) 保留 setter
 * 的值类别；不能从已禁止复制的左值按值返回。 右值 setter
 * 返回对象值，因此这里不会返回指向临时等待者的右值引用。
 */
template <class T>
  requires io::detail::io_registrant_operation<T> &&
           requires(T &&operation,
                    std::chrono::steady_clock::time_point deadline) {
             std::forward<T>(operation).set_timeout_at(deadline);
           }
decltype(auto)
timeout_at(T &&operation,
           std::chrono::steady_clock::time_point
               deadline) noexcept(noexcept(std::forward<T>(operation)
                                               .set_timeout_at(deadline))) {
  return std::forward<T>(operation).set_timeout_at(deadline);
}

/** @brief 为 IO 操作保存相对 deadline，保持右值拥有与左值借用契约。
 * @param operation 完整或紧凑策略的等待者；不会复制不可复制的等待者。
 * @param interval 从调用时起计算的毫秒时长，不会推迟至 await 时重新计时。
 * @return 右值输入返回拥有型操作值，左值输入返回原操作引用。
 */
template <class T>
  requires io::detail::io_registrant_operation<T> &&
           requires(T &&operation, std::chrono::milliseconds interval) {
             std::forward<T>(operation).set_timeout(interval);
           }
decltype(auto)
timeout(T &&operation, std::chrono::milliseconds interval) noexcept(
    noexcept(std::forward<T>(operation).set_timeout(interval))) {
  return std::forward<T>(operation).set_timeout(interval);
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