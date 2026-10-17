/**
 * @file faio/detail/io/awaiter/readv.hpp
 * @brief 跨平台异步 IO：分散读取。
 * @details 构造阶段只保存参数，真正准备与提交发生在挂起阶段。借用负载与输出存储须保持到安全完成，立即完成和跨线程完成通过统一握手避免重复恢复。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <cstring>
#include <fcntl.h>

namespace faio::io::detail {
/** @brief ReadV 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class ReadV : public IORegistrantAwaiter<ReadV> {
  using Base = IORegistrantAwaiter<ReadV>;  ///< 统一注册等待器基类，复用提交、取消与完成握手。

 public:
  ReadV(native_descriptor fd,
        const iovec* vectors,
        unsigned count,
        std::uint64_t offset,
        int flags = 0)
      : Base{[&] {
          /// @brief 构造具有确定默认状态的请求描述，随后只设置当前操作真正需要的参数。
          io_request r;
          /// @brief 固定本次操作种类，域与后端据此选择对应的方向执行权及原生提交路径。
          r.kind = operation_kind::readv;
#if defined(_WIN32)
          /// @brief 明确原生句柄种类，避免 Windows 文件 HANDLE 与 SOCKET 被错误互换。
          r.native_kind = native_handle_kind::windows_handle;
          /// @brief 声明句柄生命期已由外层租约覆盖，不为同一借用句柄登记第二份拥有者。
          r.bypass_resource_registration = true;
          /// @brief 整个 vectored 请求保留同一 OS 游标锁。
          r.windows_implicit_cursor =
              offset == UINT64_MAX;
#endif
          /// @brief 保存完整宽度的原生句柄；构造阶段不登记句柄或发起系统调用。
          r.fd = fd;
          if (count > native_iov_limit || count > INT_MAX)
            /// @brief 保存参数验证错误，在等待入口交付；非法参数不会触发系统调用。
            r.validation_error = EINVAL;
          else if (count && !vectors)
            /// @brief 保存参数验证错误，在等待入口交付；非法参数不会触发系统调用。
            r.validation_error = EFAULT;
          else if (count)
            r.vectors.assign(vectors, vectors + count);
          /// @brief 保存定位偏移或流式哨兵，不在构造阶段改变文件游标。
          r.offset = offset;
          /// @brief 保留调用方原生操作标志，平台层对不能等价实现的标志明确拒绝。
          r.flags = flags;
          /// @brief 按值交出完整初始化的请求参数；真正取得稳定操作槽与提交 IO 仍发生在 await_suspend。
          return r;
        }()} {
  }

  ReadV(resource_ptr resource,
        const iovec* vectors,
        unsigned count,
        std::uint64_t offset,
        int flags = 0)
      : ReadV{resource ? resource->fd() : -1, vectors, count, offset, flags} {
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
