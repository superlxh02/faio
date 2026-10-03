#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <fcntl.h>
namespace faio::io::detail {
// 与原生 SOCKET_URING_OP_* 保持数值兼容，不能把 GET=2 误路由到 SET。
inline constexpr int socket_inq_command = 0, socket_outq_command = 1,
                     socket_getsockopt_command = 2,
                     socket_setsockopt_command = 3;
/** @brief CmdSock 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class CmdSock : public IORegistrantAwaiter<CmdSock> {
  using Base = IORegistrantAwaiter<CmdSock>;

public:
  CmdSock(int command, int fd, int level, int option, void *value, int length)
      : Base{[&] {
          io_request r;
          r.kind =
              command == socket_getsockopt_command ? operation_kind::getsockopt
              : command == socket_setsockopt_command
                  ? operation_kind::setsockopt
              : command == socket_inq_command  ? operation_kind::socket_inq
              : command == socket_outq_command ? operation_kind::socket_outq
                                               : operation_kind::unsupported;
          r.fd = fd;
          r.argument = level;
          r.argument2 = option;
          r.buffer = value;
          r.const_buffer = value;
          if (length < 0)
            r.validation_error = EINVAL;
          else
            r.length = static_cast<std::size_t>(length);
          return r;
        }()} {}

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result < 0)
      return std::unexpected{Error{static_cast<int>(-this->_user_data.result),
                                   this->_user_data.transferred}};
    return static_cast<std::size_t>(this->_user_data.result);
  }
};
} // namespace faio::io::detail
