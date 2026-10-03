#ifndef FAIO_DETAIL_TIME_SLEEP_HPP
#define FAIO_DETAIL_TIME_SLEEP_HPP

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/task_context.hpp"
#include "faio/detail/runtime/timer/timer.hpp"
#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <optional>
#include <stop_token>

namespace faio::time::detail {
class Sleep {
 public:
  explicit Sleep(std::chrono::steady_clock::time_point deadline) : _deadline(deadline) {}

  Sleep(Sleep&& other) noexcept
      : _deadline(other._deadline), _cancelled_immediate(other._cancelled_immediate) {}

  Sleep(const Sleep&) = delete;

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
  auto await_suspend(std::coroutine_handle<> handle) -> bool {
    auto token = ::faio::detail::current_stop_token;
    if (!token.stop_possible()) {
      runtime::detail::timer::current_timer->add_task(_deadline, handle);
      return true;
    }
    if (token.stop_requested()) {
      _cancelled_immediate = true;
      return false;
    }
    // 0=注册中、1=已挂起、2=取消、3=到期。注册阶段收到 stop
    // 只改状态，await_suspend 自己返回 false；挂起后 callback 才投递恢复。
    _claim = std::make_shared<std::atomic<unsigned char>>(0);
    auto* timer = runtime::detail::timer::current_timer;
    _callback.emplace(token,
                      cancel_callback{_claim, timer, handle, ::faio::detail::current_scheduler()});
    timer->add_task(_deadline, handle, _claim);
    unsigned char expected = 0;
    if (_claim->compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
      return true;
    timer->request_prune();  // 注册期间已经收到 stop，请尽快清理新增的项。
    return false;
  }

  /// 到期返回；停止请求先获胜时抛 operation_cancelled，落选 select
  /// 分支因此不会继续执行 sleep 后面的用户代码。
  auto await_resume() const -> void {
    if (_cancelled_immediate || (_claim && _claim->load(std::memory_order_acquire) == 2))
      throw operation_cancelled{};
  }

 private:
  struct cancel_callback {
    std::shared_ptr<std::atomic<unsigned char>> claim;
    runtime::detail::timer::Timer* timer;
    std::coroutine_handle<> handle;
    scheduler_ref scheduler;

    void operator()() const noexcept {
      unsigned char state = claim->load(std::memory_order_acquire);
      while (state <= 1) {
        if (claim->compare_exchange_weak(state, 2, std::memory_order_acq_rel)) {
          if (state == 1) {
            timer->request_prune();
            scheduler.schedule(handle);
          }
          return;
        }
      }
    }
  };

  std::chrono::steady_clock::time_point _deadline;
  bool _cancelled_immediate{};
  std::shared_ptr<std::atomic<unsigned char>> _claim;
  std::optional<std::stop_callback<cancel_callback>> _callback;
};
}  // namespace faio::time::detail

#endif  // FAIO_DETAIL_TIME_SLEEP_HPP
