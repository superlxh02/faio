/**
 * @file faio/detail/io/testing/reactor_control_policy.hpp
 * @brief 仅专用测试启用的具体暂停策略。
 */
#pragma once
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
#include "faio/detail/io/base/reactor.hpp"
#include "faio/detail/io/testing/control_ref.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace faio::io::detail {
/** @brief 就绪反应器的控制通知策略。 */
class reactor_control_policy {
 public:
  /** @brief 构造就绪反应器的控制通知策略。 */
  explicit reactor_control_policy(reactor_control_test_point point) noexcept : point_(point) {}

  /** @brief 到达指定测试点时暂停控制通知。 */
  void step(reactor_control_test_point point) noexcept {
    if (point != point_)
      return;
    std::unique_lock lock(mutex_);
    if (!armed_)
      return;
    armed_ = false;
    entered_ = true;
    changed_.notify_all();
    if (!changed_.wait_for(lock, std::chrono::seconds(2), [&] { return released_; }))
      timed_out_ = true;
  }

  /** @brief 等待驱动进入指定控制通知测试点。 */
  bool wait_for_entry() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(1), [&] { return entered_; });
  }

  /** @brief 放行已暂停的控制通知。 */
  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

  /** @brief 检查等待测试放行是否超时。 */
  bool timed_out() {
    std::lock_guard lock(mutex_);
    return timed_out_;
  }

 private:
  const reactor_control_test_point point_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool armed_{true}, entered_{}, released_{}, timed_out_{};
};
}  // namespace faio::io::detail
#endif
