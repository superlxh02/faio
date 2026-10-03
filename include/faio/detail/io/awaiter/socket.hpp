#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
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

  auto await_resume() const noexcept -> expected<int> {
    if (this->_user_data.result < 0)
      return std::unexpected{Error{static_cast<int>(-this->_user_data.result),
                                   this->_user_data.transferred}};
    return static_cast<int>(this->_user_data.result);
  }
};
} // namespace faio::io::detail
