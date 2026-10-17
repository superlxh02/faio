/**
 * @file faio/detail/io/awaiter/socket.hpp
 * @brief 跨平台异步 IO：socket 资源封装。
 * @details 构造阶段只保存参数，真正准备与提交发生在挂起阶段。借用负载与输出存储须保持到安全完成，立即完成和跨线程完成通过统一握手避免重复恢复。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif
#include <fcntl.h>

namespace faio::io::detail {
/** @brief Socket 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class Socket : public IORegistrantAwaiter<Socket> {
  using Base = IORegistrantAwaiter<Socket>;  ///< 统一注册等待器基类，复用提交、取消与完成握手。

 public:
  Socket(int domain, int type, int protocol, unsigned flags)
      : Base{[&] {
          /// @brief 构造具有确定默认状态的请求描述，随后只设置当前操作真正需要的参数。
          io_request r;
          /// @brief 固定本次操作种类，域与后端据此选择对应的方向执行权及原生提交路径。
          r.kind = operation_kind::socket;
          /// @brief 保存操作专用的首个控制参数，真正执行时按当前操作种类解释。
          r.argument = domain;
          r.argument2 = type;
          r.argument3 = protocol;
          /// @brief 保留调用方原生操作标志，平台层对不能等价实现的标志明确拒绝。
          r.flags = static_cast<int>(flags);
          /// @brief 按值交出完整初始化的请求参数；真正取得稳定操作槽与提交 IO 仍发生在 await_suspend。
          return r;
        }()} {}

  /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
  auto await_resume() const noexcept -> expected<native_descriptor> {
    /// @brief 统一完成结果为负时按实际错误域解码，同时保留请求已经完成的字节进度。
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return static_cast<native_descriptor>(this->_user_data.result);
  }
};
}
