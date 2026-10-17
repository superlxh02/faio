/**
 * @file faio/detail/io/awaiter/cmd_sock.hpp
 * @brief 跨平台异步 IO：原生 socket 命令。
 * @details 构造阶段只保存参数，真正准备与提交发生在挂起阶段。借用负载与输出存储须保持到安全完成，立即完成和跨线程完成通过统一握手避免重复恢复。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif
#include <fcntl.h>

namespace faio::io::detail {
/// @brief 与原生 SOCKET_URING_OP_* 保持数值兼容，不能把 GET=2 误路由到 SET。
inline constexpr int socket_inq_command = 0, socket_outq_command = 1, socket_getsockopt_command = 2,
                     socket_setsockopt_command = 3;

/** @brief CmdSock 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class CmdSock : public IORegistrantAwaiter<CmdSock> {
  using Base = IORegistrantAwaiter<CmdSock>;  ///< 统一注册等待器基类，复用提交、取消与完成握手。

 public:
  CmdSock(int command, native_descriptor fd, int level, int option, void* value, int length)
      : Base{[&] {
          /// @brief 构造具有确定默认状态的请求描述，随后只设置当前操作真正需要的参数。
          io_request r;
          /// @brief 固定本次操作种类，域与后端据此选择对应的方向执行权及原生提交路径。
          r.kind = command == socket_getsockopt_command   ? operation_kind::getsockopt
                   : command == socket_setsockopt_command ? operation_kind::setsockopt
                   : command == socket_inq_command        ? operation_kind::socket_inq
                   : command == socket_outq_command       ? operation_kind::socket_outq
                                                          : operation_kind::unsupported;
          /// @brief 保存完整宽度的原生句柄；构造阶段不登记句柄或发起系统调用。
          r.fd = fd;
          /// @brief 保存操作专用的首个控制参数，真正执行时按当前操作种类解释。
          r.argument = level;
          r.argument2 = option;
          /// @brief 仅保存可写缓冲区借用；调用方在安全完成前不得释放或调整其地址。
          r.buffer = value;
          /// @brief 仅保存只读负载借用；取消请求也不能提前解除内核对负载的借用。
          r.const_buffer = value;
          if (length < 0)
            /// @brief 保存参数验证错误，在等待入口交付；非法参数不会触发系统调用。
            r.validation_error = EINVAL;
          else
            /// @brief 记录本次传输字节容量，实际完成仍按真实短读写结果报告。
            r.length = static_cast<std::size_t>(length);
          /// @brief 按值交出完整初始化的请求参数；真正取得稳定操作槽与提交 IO 仍发生在 await_suspend。
          return r;
        }()} {}

  /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
  auto await_resume() const noexcept -> expected<std::size_t> {
    /// @brief 统一完成结果为负时按实际错误域解码，同时保留请求已经完成的字节进度。
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    /// @brief 成功时返回真实完成字节数，零读与短读写由上层按对应流或报文语义处理。
    return static_cast<std::size_t>(this->_user_data.result);
  }
};
}
