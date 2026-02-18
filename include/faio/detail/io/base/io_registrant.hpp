#ifndef FAIO_DETAIL_IO_BASE_IO_REGISTRANT_HPP
#define FAIO_DETAIL_IO_BASE_IO_REGISTRANT_HPP
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/uring/io_uring.hpp"
#include "faio/detail/io/uring/io_user_data.hpp"
#include "faio/detail/time/timeout.hpp"
#include <functional>
#include <liburing.h>
#include <utility>
namespace faio::io::detail {

// IO操作注册器，通过构造函数传入IO操作函数和参数。
// 将io操作注册到uring里，并设置用户数据。
// 将IO操作封装成awaiter。
template <class IO> class IORegistrantAwaiter {
public:
  template <typename F, typename... Args>
    requires std::is_invocable_v<F, io_uring_sqe *, Args...>
  IORegistrantAwaiter(F &&f, Args &&...args) : _sqe(current_uring->get_sqe()) {
    if (_sqe != nullptr) {
      // 调用函数（io_uring_prep_* 会将 sqe->user_data 置零）
      std::invoke(std::forward<F>(f), _sqe, std::forward<Args>(args)...);
      // 必须在 prep 之后设置 user_data，否则完成事件无法关联回协程
      io_uring_sqe_set_data(_sqe, &_user_data);
    } else {
      _user_data.result = -Error::EmptySqe;
    }
  }

  IORegistrantAwaiter(const IORegistrantAwaiter &) = delete;
  IORegistrantAwaiter &operator=(const IORegistrantAwaiter &) = delete;
  IORegistrantAwaiter(IORegistrantAwaiter &&other)
      : _user_data(std::move(other._user_data)), _sqe(other._sqe) {
    io_uring_sqe_set_data(_sqe, &this->_user_data);
    other._sqe = nullptr;
  }
  IORegistrantAwaiter &operator=(IORegistrantAwaiter &&other) {
    _user_data = std::move(other._user_data);
    _sqe = other._sqe;
    io_uring_sqe_set_data(_sqe, &this->_user_data);
    other._sqe = nullptr;
    return *this;
  };

  ~IORegistrantAwaiter() = default;

public:
  // 是否挂起逻辑，如果sqe不为空就挂起
  bool await_ready() const noexcept { return _sqe == nullptr; }

  // 挂起逻辑，设置用户数据和提交io请求
  void await_suspend(std::coroutine_handle<> handle) {
    _user_data.handle = std::move(handle);
    io::detail::current_uring->submit();
  }

public:
  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) noexcept {
    _user_data.deadline = deadline;
    return time::detail::Timeout{std::move(*static_cast<IO *>(this))};
  }

  auto set_timeout(std::chrono::milliseconds interval) noexcept {
    return set_timeout_at(std::chrono::steady_clock::now() + interval);
  }

protected:
  io_user_data_t _user_data{};
  io_uring_sqe *_sqe;
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_BASE_IO_REGISTRANT_HPP