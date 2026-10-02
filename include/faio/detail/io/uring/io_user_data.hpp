#ifndef FAIO_DETAIL_IO_BASE_IO_DATA_HPP
#define FAIO_DETAIL_IO_BASE_IO_DATA_HPP

#include <chrono>
#include <coroutine>
#include <atomic>
#include <memory>
namespace faio::runtime::detail::timer {
class TimerTask;

}

namespace faio::io::detail {
struct io_user_data_t;
struct io_cancel_state {
  std::atomic<bool> done{false};
  io_user_data_t* target{}; // 只由所属 io_engine 读取；done 后禁止解引用。
};
struct io_user_data_t {
  std::coroutine_handle<> handle{nullptr};                      // 协程句柄
  int result;                                                   // 结果
  faio::runtime::detail::timer::TimerTask *timer_task{nullptr}; // 定时器任务
  std::chrono::steady_clock::time_point deadline;               // 截止时间
  std::shared_ptr<io_cancel_state> cancel_state;
};
} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_BASE_IO_DATA_HPP
