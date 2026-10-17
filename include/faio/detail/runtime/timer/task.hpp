/**
 * @file faio/detail/runtime/timer/task.hpp
 * @brief 时间轮定时节点、取消仲裁与到期就绪发布。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#ifndef FAIO_DETAIL_RUNTIME_TIMER_TASK_HPP
#define FAIO_DETAIL_RUNTIME_TIMER_TASK_HPP

#include "faio/detail/coroutine/scheduler.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <memory>

namespace faio::runtime::detail::timer {
/// @brief 封装定时器任务实体类
///
/// sleep 持有 coroutine_handle，到期只交给调度器排队。
/// IO deadline 在统一 domain 里按代际 token 取消，与 sleep 时间轮分离。
class TimerTask {
 public:
  TimerTask(std::chrono::steady_clock::time_point deadline,
            std::coroutine_handle<> handle,
            std::shared_ptr<std::atomic<unsigned char>> claim = {})
      : _handle(handle), _deadline(deadline), _claim(std::move(claim)) {}

 public:
  bool cancelled() const noexcept { return _claim && _claim->load(std::memory_order_acquire) == 2; }

  /// @brief 执行到期的定时器任务
  ///
  /// sleep 路径：将协程句柄推入本地任务队列等待调度
  /// IO 操作的超时由中立 domain 处理，此类型不持有 IO 操作裸指针。
  template <ready_sink sink_type>
  void execute(sink_type& sink) {
    /// @brief 只有携带有效恢复目标的定时节点才参与到期交接。
    if (_handle != nullptr) {
      /// @brief sleep 路径：竞争到期恢复权，成功后只发布就绪句柄
      /// stop callback 与超时路径竞争唯一恢复权。取消后帧可能已释放，
      /// 此时只销毁定时器项，绝不再访问协程句柄。
      unsigned char expected = 1;
      /// @brief 未使用共享仲裁的节点直接发布；使用仲裁时仅到期 CAS 的胜者可投递句柄，取消胜出后不再访问其帧。
      if (!_claim || _claim->compare_exchange_strong(expected, 3, std::memory_order_acq_rel))
        /// @brief 只把句柄交给所属线程的就绪接收端，不在时间轮扫描内直接恢复用户协程。
        sink.enqueue_ready(_handle);
    }
  }

 public:
  std::coroutine_handle<> _handle{nullptr};  ///< 协程句柄（sleep 场景）
  std::chrono::steady_clock::time_point _deadline;  ///< 截止时间
  std::unique_ptr<TimerTask> _next{nullptr};  ///< 链表下一个节点
  std::shared_ptr<std::atomic<unsigned char>> _claim;  ///< 到期与取消共享的原子领取状态，确保协程只恢复一次。
};
}

#endif
