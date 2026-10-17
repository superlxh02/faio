/**
 * @file faio/detail/time/sleep.hpp
 * @brief 异步时间操作：可取消异步休眠。
 * @details 使用单调时钟推进异步等待；停止与到期共享唯一完成仲裁。组合超时在返回前排空已启动的分支，避免协程帧和借用参数提前失效。
 */
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

  /// @brief 如果 deadline 已经过期或恰好到期，则无需挂起
  auto await_ready() const noexcept -> bool {
    return _deadline <= std::chrono::steady_clock::now();
  }

  /// @brief 将协程注册到定时器，在 deadline 到达时恢复
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
    /// @brief 0=注册中、1=已挂起、2=取消、3=到期。注册阶段收到 stop
    /// 只改状态，await_suspend 自己返回 false；挂起后 callback 才投递恢复。
    _claim = std::make_shared<std::atomic<unsigned char>>(0);
    auto* timer = runtime::detail::timer::current_timer;
    _callback.emplace(token,
                      cancel_callback{_claim, timer, handle, ::faio::detail::current_scheduler()});
    timer->add_task(_deadline, handle, _claim);
    unsigned char expected = 0;
    if (_claim->compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
      return true;
    /// @brief 注册期间已经收到 stop，请尽快清理新增的项。
    timer->request_prune();
    return false;
  }

  /// @brief 到期返回；停止请求先获胜时抛 operation_cancelled，落选 select
  /// 分支因此不会继续执行 sleep 后面的用户代码。
  auto await_resume() const -> void {
    if (_cancelled_immediate || (_claim && _claim->load(std::memory_order_acquire) == 2))
      throw operation_cancelled{};
  }

 private:
  struct cancel_callback {
    std::shared_ptr<std::atomic<unsigned char>> claim;  ///< 到期与取消共享的原子领取状态，确保协程只恢复一次。
    runtime::detail::timer::Timer* timer;  ///< 关联的时间轮管理器，登记和取消须遵循所属线程协议。
    std::coroutine_handle<> handle;  ///< 当前持有或等待恢复的协程帧句柄，所有权遵循所属等待协议。
    scheduler_ref scheduler;  ///< 捕获的目标调度器，跨线程完成时将续体投递回所属运行时。

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

  std::chrono::steady_clock::time_point _deadline;  ///< 下一次计划触发的单调时钟绝对时间点。
  bool _cancelled_immediate{};  ///< 挂起登记前是否已经观察到停止请求。
  std::shared_ptr<std::atomic<unsigned char>> _claim;  ///< 到期与取消共享的原子领取状态，确保协程只恢复一次。
  std::optional<std::stop_callback<cancel_callback>> _callback;  ///< 按需就地保存的停止回调，析构时注销并等待在途回调结束。
};
}
#endif
