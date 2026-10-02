#ifndef FAIO_DETAIL_IO_BASE_IO_REGISTRANT_HPP
#define FAIO_DETAIL_IO_BASE_IO_REGISTRANT_HPP
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/uring/io_uring.hpp"
#include "faio/detail/runtime/core/io_engine.hpp"
#include "faio/detail/coroutine/task_context.hpp"
#include "faio/detail/io/uring/io_user_data.hpp"
#include "faio/detail/time/timeout.hpp"
#include <functional>
#include <liburing.h>
#include <utility>
#include <memory>
#include <optional>
#include <stop_token>
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
    if (_sqe != nullptr) io_uring_sqe_set_data(_sqe, &this->_user_data);
    other._sqe = nullptr;
  }
  IORegistrantAwaiter &operator=(IORegistrantAwaiter &&other) {
    _stop_callback.reset(); // 只能在 await_suspend 之前移动 awaiter。
    _user_data = std::move(other._user_data);
    _sqe = other._sqe;
    if (_sqe != nullptr) io_uring_sqe_set_data(_sqe, &this->_user_data);
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
    auto token = ::faio::detail::current_stop_token;
    if (token.stop_possible()) {
      // SQE 在构造函数已登记 user_data；此处若分配失败就不能安全销毁帧，
      // 因为尚未提交的 SQE 仍持有帧内地址。明确终止，避免隐蔽的 UAF。
      try {
        _user_data.cancel_state = std::make_shared<io_cancel_state>();
        _user_data.cancel_state->target = &_user_data;
        _stop_callback.emplace(token, cancel_callback{_user_data.cancel_state,
                                                        runtime::detail::current_io_engine});
      } catch (...) { std::terminate(); }
    }
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
  struct cancel_callback {
    std::shared_ptr<io_cancel_state> state;
    runtime::detail::io_engine* engine;
    void operator()() const noexcept { engine->request_cancel(state); }
  };
  io_user_data_t _user_data{};
  io_uring_sqe *_sqe;
  std::optional<std::stop_callback<cancel_callback>> _stop_callback;
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_BASE_IO_REGISTRANT_HPP
