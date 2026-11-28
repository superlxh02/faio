/**
 * @file faio/detail/io/awaiter/cmd_sock.hpp
 * @brief 原生套接字命令。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif

namespace faio::io::detail {
/** @brief 与原生 SOCKET_URING_OP_* 保持数值兼容，不能把 GET=2 误路由到 SET。 */
inline constexpr int socket_inq_command = 0, socket_outq_command = 1, socket_getsockopt_command = 2,
                     socket_setsockopt_command = 3;

/**  @brief CmdSock 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class cmd_sock : public io_registrant_awaiter<cmd_sock> {
  using base_type =
      io_registrant_awaiter<cmd_sock>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造套接字控制等待器。 */
  cmd_sock(int command, native_descriptor fd, int level, int option, void* value, int length)
      : base_type{[&] {
          io_request r;
          r.kind = command == socket_getsockopt_command   ? operation_kind::GETSOCKOPT
                   : command == socket_setsockopt_command ? operation_kind::SETSOCKOPT
                   : command == socket_inq_command        ? operation_kind::SOCKET_INQ
                   : command == socket_outq_command       ? operation_kind::SOCKET_OUTQ
                                                          : operation_kind::UNSUPPORTED;
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

  /** @brief 取得异步操作结果。 */
  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return static_cast<std::size_t>(this->_user_data.result);
  }
};
}  // namespace faio::io::detail

namespace faio::io {
/**  @brief 操作工厂与等待器在同一头文件中定义，按需包含即可，无需引入其他 I/O 操作。 */
inline auto getsockopt(
    detail::native_descriptor fd, int level, int option, void* value, int length) {
  return detail::cmd_sock{detail::socket_getsockopt_command, fd, level, option, value, length};
}

/** @brief 设置原生套接字选项。 */
inline auto setsockopt(
    detail::native_descriptor fd, int level, int option, void* value, int length) {
  return detail::cmd_sock{detail::socket_setsockopt_command, fd, level, option, value, length};
}

/** @brief 提交原生套接字控制请求。 */
inline auto cmdsock(
    int command, detail::native_descriptor fd, int level, int option, void* value, int length) {
  return detail::cmd_sock{command, fd, level, option, value, length};
}
}  // namespace faio::io
