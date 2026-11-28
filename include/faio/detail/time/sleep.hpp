/**
 * @file faio/detail/time/sleep.hpp
 * @brief 可取消的异步休眠。
 */
#ifndef FAIO_DETAIL_TIME_SLEEP_HPP
#define FAIO_DETAIL_TIME_SLEEP_HPP

#include "faio/detail/coroutine/context/current_context.hpp"
#include "faio/detail/runtime/common/coroutine_integration.hpp"
#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/context/task_context.hpp"
#include "faio/detail/coroutine/scheduler/resume_target.hpp"
#include "faio/detail/runtime/timer/timer.hpp"
#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>

namespace faio::time::detail {
/** @brief 支持取消的定时等待器。 */
template <class S = default_scheduler>
class basic_sleep {
 public:
  using scheduler_type = S;

  /** @brief 构造支持取消的定时等待器。 */
  explicit basic_sleep(std::chrono::steady_clock::time_point deadline,
                       scheduler_ref_for<S> scheduler = {})
      : _deadline(deadline), _scheduler(scheduler) {}

  /** @brief 移动构造支持取消的定时等待器。 */
  basic_sleep(basic_sleep&& other) noexcept
      : _deadline(other._deadline),
        _scheduler(other._scheduler),
        _cancelled_immediate(other._cancelled_immediate) {}

  /** @brief 禁止复制支持取消的定时等待器。 */
  basic_sleep(const basic_sleep&) = delete;

  /** @brief 在登记定时任务前，根据 promise 的调度器类型设置取消后的恢复目标。 */
  template <class Target>
  auto rebind_scheduler(scheduler_ref_for<Target> scheduler) const {
    if (_started)
      throw std::logic_error("不能重新绑定已经登记的 sleep 等待器");
    return basic_sleep<Target>{_deadline, scheduler};
  }

  /** @brief 绑定等待器的静态恢复调度器。 */
  template <class Target>
  auto rebind_scheduler() const {
    return rebind_scheduler<Target>(::faio::detail::current_scheduler_for<Target>());
  }

 public:
  /** @brief 读取单调时钟的截止时间。 */
  [[nodiscard]]
  auto deadline() const noexcept -> std::chrono::steady_clock::time_point {
    return _deadline;
  }

  /** @brief deadline 已到达时无需挂起。 */
  auto await_ready() const noexcept -> bool {
    return _deadline <= std::chrono::steady_clock::now();
  }

  /** @brief 将协程登记到定时器，在 deadline 到期时恢复。 */
  auto await_suspend(std::coroutine_handle<> handle) -> bool {
    auto* timer = ::faio::detail::current_thread_context().io.timer;
    if (!timer)
      throw std::logic_error("sleep 需要当前线程的定时器绑定");
    if (!_scheduler)
      _scheduler = ::faio::detail::require_current_scheduler_for<S>();
    const auto target = ::faio::detail::capture_resume_target(_scheduler);
    _started = true;
    auto token = ::faio::detail::current_task_context().stop_token;
    if (!token.stop_possible()) {
      timer->add_task(_deadline, handle, {}, target);
      return true;
    }
    if (token.stop_requested()) {
      _cancelled_immediate = true;
      return false;
    }
    _claim = std::make_shared<std::atomic<unsigned char>>(0);
    _callback.emplace(token, cancel_callback{_claim, timer, handle, target});
    timer->add_task(_deadline, handle, _claim, target);
    unsigned char expected = 0;
    if (_claim->compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
      return true;
    timer->request_prune();
    return false;
  }

  /**
   * @brief 到期时返回；若停止请求先获胜，则抛出 operation_cancelled，
   * 使 select 中未选中的分支不再执行 sleep 之后的用户代码。
   */
  auto await_resume() const -> void {
    if (_cancelled_immediate || (_claim && _claim->load(std::memory_order_acquire) == 2))
      throw operation_cancelled{};
  }

 private:
  /** @brief 协作取消回调。 */
  struct cancel_callback {
    std::shared_ptr<std::atomic<unsigned char>>
        claim;                            /**< 到期与取消共享的原子领取状态，确保协程只恢复一次。 */
    runtime::detail::timer::timer* timer; /**< 关联的时间轮管理器，登记和取消须遵循所属线程协议。 */
    std::coroutine_handle<> handle;       /**< 等待恢复的协程句柄，所有权由等待协议规定。 */
    ::faio::detail::basic_resume_target<S> scheduler; /**< 到期和取消共用捕获的恢复目标。 */

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

  std::chrono::steady_clock::time_point _deadline; /**< 计划唤醒的单调时钟绝对时间点。 */
  bool _started{};
  scheduler_ref_for<S> _scheduler;
  bool _cancelled_immediate{}; /**< 挂起登记前是否已经观察到停止请求。 */
  std::shared_ptr<std::atomic<unsigned char>>
      _claim; /**< 到期与取消共享的原子领取状态，确保协程只恢复一次。 */
  std::optional<std::stop_callback<cancel_callback>>
      _callback; /**< 按需就地保存停止回调，析构时注销并等待仍在执行的回调结束。 */
};

using sleep_awaiter = basic_sleep<>;
}  // namespace faio::time::detail
#endif
