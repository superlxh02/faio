#ifndef FAIO_DETAIL_IO_BASE_IO_DATA_HPP
#define FAIO_DETAIL_IO_BASE_IO_DATA_HPP

#include <chrono>
#include <coroutine>
namespace faio::runtime::detail::timer {
class TimerTask;

}

namespace faio::io::detail {
struct io_user_data_t {
  std::coroutine_handle<> handle{nullptr};                      // 协程句柄
  int result;                                                   // 结果
  faio::runtime::detail::timer::TimerTask *timer_task{nullptr}; // 定时器任务
  std::chrono::steady_clock::time_point deadline;               // 截止时间
};
} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_BASE_IO_DATA_HPP