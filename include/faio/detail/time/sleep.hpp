#ifndef FAIO_DETAIL_TIME_SLEEP_HPP
#define FAIO_DETAIL_TIME_SLEEP_HPP

#include "faio/detail/runtime/core/timer/timer.hpp"
#include <chrono>
#include <coroutine>

namespace faio::time::detail {
class Sleep {
public:
  explicit Sleep(std::chrono::steady_clock::time_point deadline)
      : _deadline(deadline) {}

public:
  [[nodiscard]]
  auto deadline() const noexcept -> std::chrono::steady_clock::time_point {
    return _deadline;
  }

  /// 如果 deadline 已经过期或恰好到期，则无需挂起
  auto await_ready() const noexcept -> bool {
    return _deadline <= std::chrono::steady_clock::now();
  }

  /// 将协程注册到定时器，在 deadline 到达时恢复
  auto await_suspend(std::coroutine_handle<> handle) noexcept -> void {
    runtime::detail::timer::current_timer->add_task(_deadline, handle);
  }

  /// sleep 正常唤醒，始终返回成功
  auto await_resume() const noexcept -> void {}

private:
  std::chrono::steady_clock::time_point _deadline;
};
} // namespace faio::time::detail

#endif // FAIO_DETAIL_TIME_SLEEP_HPP