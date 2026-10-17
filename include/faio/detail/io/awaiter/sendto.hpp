/**
 * @file faio/detail/io/awaiter/sendto.hpp
 * @brief 跨平台异步 IO：向对端发送数据报。
 * @details 构造阶段只保存参数，真正准备与提交发生在挂起阶段。借用负载与输出存储须保持到安全完成，立即完成和跨线程完成通过统一握手避免重复恢复。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif
#include <cstring>
#include <fcntl.h>

namespace faio::io::detail {
/** @brief SendTo 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class SendTo : public IORegistrantAwaiter<SendTo> {
  using Base = IORegistrantAwaiter<SendTo>;  ///< 统一注册等待器基类，复用提交、取消与完成握手。

 public:
  SendTo(native_descriptor fd,
         const void* buffer,
         std::size_t length,
         int flags,
         const sockaddr* address,
         socklen_t address_length)
      : Base{[&] {
          /// @brief 构造具有确定默认状态的请求描述，随后只设置当前操作真正需要的参数。
          io_request r;
          /// @brief 固定本次操作种类，域与后端据此选择对应的方向执行权及原生提交路径。
          r.kind = operation_kind::sendto;
          /// @brief 保存完整宽度的原生句柄；构造阶段不登记句柄或发起系统调用。
          r.fd = fd;
          /// @brief 仅保存只读负载借用；取消请求也不能提前解除内核对负载的借用。
          r.const_buffer = buffer;
          /// @brief 记录本次传输字节容量，实际完成仍按真实短读写结果报告。
          r.length = length;
          /// @brief 保留调用方原生操作标志，平台层对不能等价实现的标志明确拒绝。
          r.flags = flags;
          /// @brief 记录拥有地址中的有效字节长度，不能把完整存储容量当作真实地址长度。
          r.address_length = address_length;
          /// @brief 显式扩大为 size_t；Windows 负 socklen_t 转换后超容量，不发生越界复制。
          if (static_cast<std::size_t>(address_length) <= sizeof(r.address) && address)
            std::memcpy(&r.address, address, address_length);
          else
            /// @brief 固定本次操作种类，域与后端据此选择对应的方向执行权及原生提交路径。
            r.kind = operation_kind::unsupported;
          /// @brief 按值交出完整初始化的请求参数；真正取得稳定操作槽与提交 IO 仍发生在 await_suspend。
          return r;
        }()} {}

  SendTo(resource_ptr resource,
         const void* buffer,
         std::size_t length,
         int flags,
         const sockaddr* address,
         socklen_t address_length)
      : SendTo{resource ? resource->fd() : -1, buffer, length, flags, address, address_length} {
    /// @brief 保留共享资源租约，使挂起、取消与完成期间的句柄身份和域归属稳定。
    this->request_.resource = std::move(resource);
  }

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
