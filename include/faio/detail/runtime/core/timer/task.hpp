#ifndef FAIO_DETAIL_RUNTIME_CORE_TIMER_TIME_TASK_HPP
#define FAIO_DETAIL_RUNTIME_CORE_TIMER_TIME_TASK_HPP

#include "faio/detail/coroutine/scheduler.hpp"
#include "faio/detail/io/uring/io_uring.hpp"
#include "faio/detail/io/uring/io_user_data.hpp"
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <liburing.h>
#include <memory>
#include <atomic>

namespace faio::runtime::detail::timer {

// 封装定时器任务实体类
//
// 两种使用场景：
//   1. sleep：持有 coroutine_handle，到期后直接恢复协程
//   2. IO 超时：持有 io_user_data_t*，到期后取消对应的 io_uring 操作
class TimerTask {
public:
  TimerTask(std::chrono::steady_clock::time_point deadline,
            std::coroutine_handle<> handle,
            std::shared_ptr<std::atomic<unsigned char>> claim = {})
      : _handle(handle), _deadline(deadline), _claim(std::move(claim)) {}

  TimerTask(std::chrono::steady_clock::time_point deadline,
            io::detail::io_user_data_t *user_data)
      : _handle(nullptr), _deadline(deadline), _user_data(user_data) {}

public:
  bool cancelled() const noexcept {
    return _claim && _claim->load(std::memory_order_acquire) == 2;
  }
  /// 执行到期的定时器任务
  ///
  /// sleep 路径：将协程句柄推入本地任务队列等待调度
  /// IO 超时路径：设置超时错误码，通过 io_uring 取消挂起的 IO 操作
  template <ready_sink sink_type>
  void execute(sink_type &sink) {
    if (_handle != nullptr) {
      // sleep 路径：直接恢复协程
      // stop callback 与超时路径竞争唯一恢复权。取消后帧可能已释放，
      // 此时只销毁定时器项，绝不再访问协程句柄。
      unsigned char expected = 1;
      if (!_claim || _claim->compare_exchange_strong(expected, 3,
                                                      std::memory_order_acq_rel))
        sink.enqueue_ready(_handle);
    } else if (_user_data != nullptr) {
      // IO 超时路径：设置超时错误并取消 IO 操作
      _user_data->result = -ETIMEDOUT;
      _user_data->timer_task = nullptr;
      auto sqe = io::detail::current_uring->get_sqe();
      if (sqe != nullptr) {
        io_uring_prep_cancel(sqe, _user_data, 0);
        io_uring_sqe_set_data(sqe, nullptr);
      }
    }
  }

public:
  std::coroutine_handle<> _handle{nullptr};        // 协程句柄（sleep 场景）
  std::chrono::steady_clock::time_point _deadline; // 截止时间
  io::detail::io_user_data_t *_user_data{nullptr}; // 用户数据（IO 超时场景）
  std::unique_ptr<TimerTask> _next{nullptr};       // 链表下一个节点
  std::shared_ptr<std::atomic<unsigned char>> _claim;
};

} // namespace faio::runtime::detail::timer

#endif // FAIO_DETAIL_RUNTIME_CORE_TIMER_TIME_TASK_HPP
