/**
 * @file faio/detail/runtime/timer/task.hpp
 * @brief 时间轮定时节点、取消仲裁与到期就绪发布。
 */
#ifndef FAIO_DETAIL_RUNTIME_TIMER_TASK_HPP
#define FAIO_DETAIL_RUNTIME_TIMER_TASK_HPP

#include "faio/detail/coroutine/scheduler/resume_target.hpp"
#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <memory>
#include <utility>

namespace faio::runtime::detail::timer {
/**
 * @brief 定时器任务节点。
 * sleep 持有 coroutine_handle，到期只交给调度器排队。
 * I/O 截止时间在统一 domain 中按代际 token 取消，与 sleep 时间轮分开管理。
 */
class TimerTask {
  /** @brief 定时到期通知。 */
  class notification {
   public:
    /** @brief 协程返回值与挂起协议。 */
    struct promise_type {
      /** @brief 创建协程返回对象。 */
      notification get_return_object() noexcept {
        return notification{std::coroutine_handle<promise_type>::from_promise(*this)};
      }

      /** @brief 协程创建后保持挂起，等待显式启动。 */
      std::suspend_always initial_suspend() const noexcept { return {}; }

      /** @brief 完成后直接销毁协程帧。 */
      std::suspend_never final_suspend() const noexcept { return {}; }

      /** @brief 标记无返回值协程完成。 */
      void return_void() const noexcept {}

      /** @brief 未捕获异常时终止进程。 */
      void unhandled_exception() const noexcept { std::terminate(); }
    };

    /** @brief 构造定时到期通知。 */
    explicit notification(std::coroutine_handle<> handle) noexcept : handle_(handle) {}

    /** @brief 禁止复制构造定时到期通知。 */
    notification(const notification&) = delete;

    /** @brief 销毁定时到期通知。 */
    ~notification() {
      if (handle_)
        handle_.destroy();
    }

    /** @brief 转出定时通知协程句柄。 */
    std::coroutine_handle<> release() noexcept { return std::exchange(handle_, {}); }

   private:
    std::coroutine_handle<> handle_;
  };

 public:
  /** @brief 构造定时任务节点。 */
  TimerTask(std::chrono::steady_clock::time_point deadline,
            std::coroutine_handle<> handle,
            std::shared_ptr<std::atomic<unsigned char>> claim = {})
      : _handle(handle), _deadline(deadline), _claim(std::move(claim)) {}

  /** @brief 构造定时任务节点。 */
  template <class S>
  TimerTask(std::chrono::steady_clock::time_point deadline,
            std::coroutine_handle<> handle,
            std::shared_ptr<std::atomic<unsigned char>> claim,
            ::faio::detail::basic_resume_target<S> target)
      : TimerTask(deadline, handle, std::move(claim)) {
    if constexpr (std::same_as<S, ::faio::default_scheduler>)
      target_ = target;
    else {
      auto pending = notify(target, handle);
      notification_ = pending.release();
    }
  }

  /** @brief 销毁定时任务节点。 */
  ~TimerTask() {
    if (notification_)
      notification_.destroy();
  }

 public:
  /** @brief 判断操作是否已取消。 */
  bool cancelled() const noexcept { return _claim && _claim->load(std::memory_order_acquire) == 2; }

  /**
   * @brief 执行到期的定时器任务。
   * sleep 路径将协程句柄放入本地任务队列，等待调度。
   * I/O 操作的超时由独立 domain 处理，此类型不持有 I/O 操作的裸指针。
   */
  template <ready_sink sink_type>
  void execute(sink_type& sink) {
    if (_handle != nullptr) {
      unsigned char expected = 1;
      if (!_claim || _claim->compare_exchange_strong(expected, 3, std::memory_order_acq_rel)) {
        const auto handle = _handle;
        const auto target = target_;
        if (auto pending = std::exchange(notification_, {}))
          pending.resume();
        else if (target && (target.policy().no_stealing || target.policy().binds_core()))
          target.schedule_yield(handle);
        else
          sink.enqueue_ready(handle);
      }
    }
  }

 public:
  std::coroutine_handle<> _handle{nullptr};        /**< 协程句柄（sleep 场景） */
  std::chrono::steady_clock::time_point _deadline; /**< 截止时间 */
  std::unique_ptr<TimerTask> _next{nullptr};       /**< 链表下一个节点 */
  std::shared_ptr<std::atomic<unsigned char>>
      _claim; /**< 到期与取消共享的原子领取状态，确保协程只恢复一次。 */

 private:
  /** @brief 通知等待者重新检查条件。 */
  template <class S>
  static notification notify(::faio::detail::basic_resume_target<S> target,
                             std::coroutine_handle<> handle) {
    target.schedule_yield(handle);
    co_return;
  }

  ::faio::detail::resume_target target_;
  std::coroutine_handle<> notification_{};
};
}  // namespace faio::runtime::detail::timer

#endif
