#ifndef FAIO_DETAIL_TIME_TIMEOUT_HPP
#define FAIO_DETAIL_TIME_TIMEOUT_HPP

#include "faio/detail/runtime/core/timer/timer.hpp"
#include <cassert>
#include <coroutine>

namespace faio::io::detail {
template <class T> class IORegistrantAwaiter;
}

namespace faio::time::detail {
template <class T>
  requires std::derived_from<T, io::detail::IORegistrantAwaiter<T>>
class Timeout : public T {
public:
  Timeout(T &&io) : T{std::move(io)} {}

public:
  auto await_suspend(std::coroutine_handle<> handle) -> bool {
    // 将定时器任务注册到当前 worker 的 Timer 中
    // add_task 返回 TimerTask* 裸指针（Timer 拥有所有权）
    auto *timer_task = runtime::detail::timer::current_timer->add_task(
        this->_user_data.deadline, &this->_user_data);
    this->_user_data.timer_task = timer_task;
    T::await_suspend(handle);
    return true;
  }
};
} // namespace faio::time::detail

#endif // FAIO_DETAIL_TIME_TIMEOUT_HPP