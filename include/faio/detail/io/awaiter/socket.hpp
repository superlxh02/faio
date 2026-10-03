#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows_error.hpp"
#endif
#include <fcntl.h>

namespace faio::io::detail {
/** @brief Socket 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class Socket : public IORegistrantAwaiter<Socket> {
  using Base = IORegistrantAwaiter<Socket>;

 public:
  Socket(int domain, int type, int protocol, unsigned flags)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::socket;
          r.argument = domain;
          r.argument2 = type;
          r.argument3 = protocol;
          r.flags = static_cast<int>(flags);
          return r;
        }()} {}

  auto await_resume() const noexcept -> expected<native_descriptor> {
    if (this->_user_data.result < 0)
#if defined(_WIN32)
      return std::unexpected{windows::make_io_error(static_cast<int>(-this->_user_data.result),
                                                    this->_user_data.transferred)};
#else
      return std::unexpected{
          Error{static_cast<int>(-this->_user_data.result), this->_user_data.transferred}};
#endif
    return static_cast<native_descriptor>(this->_user_data.result);
  }
};
}  // namespace faio::io::detail
