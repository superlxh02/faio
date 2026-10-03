#ifndef FAIO_DETAIL_RUNTIME_TIMER_TASK_HPP
#define FAIO_DETAIL_RUNTIME_TIMER_TASK_HPP

#include "faio/detail/coroutine/scheduler.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <memory>

namespace faio::runtime::detail::timer {
// 封装定时器任务实体类
//
// sleep 持有 coroutine_handle，到期只交给调度器排队。
// IO deadline 在统一 domain 里按代际 token 取消，与 sleep 时间轮分离。
class TimerTask {
 public:
  TimerTask(std::chrono::steady_clock::time_point deadline,
            std::coroutine_handle<> handle,
            std::shared_ptr<std::atomic<unsigned char>> claim = {})
      : _handle(handle), _deadline(deadline), _claim(std::move(claim)) {}

 public:
  bool cancelled() const noexcept { return _claim && _claim->load(std::memory_order_acquire) == 2; }

  /// 执行到期的定时器任务
  ///
  /// sleep 路径：将协程句柄推入本地任务队列等待调度
  /// IO 操作的超时由中立 domain 处理，此类型不持有 IO 操作裸指针。
  template <ready_sink sink_type>
  void execute(sink_type& sink) {
    if (_handle != nullptr) {
      // sleep 路径：直接恢复协程
      // stop callback 与超时路径竞争唯一恢复权。取消后帧可能已释放，
      // 此时只销毁定时器项，绝不再访问协程句柄。
      unsigned char expected = 1;
      if (!_claim || _claim->compare_exchange_strong(expected, 3, std::memory_order_acq_rel))
        sink.enqueue_ready(_handle);
    }
  }

 public:
  std::coroutine_handle<> _handle{nullptr};         // 协程句柄（sleep 场景）
  std::chrono::steady_clock::time_point _deadline;  // 截止时间
  std::unique_ptr<TimerTask> _next{nullptr};        // 链表下一个节点
  std::shared_ptr<std::atomic<unsigned char>> _claim;
};
}  // namespace faio::runtime::detail::timer

#endif
