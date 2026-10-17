/**
 * @file faio/detail/io/awaiter/close.hpp
 * @brief 跨平台异步 IO：安全关闭。
 * @details 构造阶段只保存参数，真正准备与提交发生在挂起阶段。借用负载与输出存储须保持到安全完成，立即完成和跨线程完成通过统一握手避免重复恢复。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <fcntl.h>

namespace faio::io::detail {
/** @brief Close 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class Close : public IORegistrantAwaiter<Close> {
  using Base = IORegistrantAwaiter<Close>;  ///< 统一注册等待器基类，复用提交、取消与完成握手。

 public:
  Close(native_descriptor fd)
      : Base{[&] {
          /// @brief 构造具有确定默认状态的请求描述，随后只设置当前操作真正需要的参数。
          io_request r;
          /// @brief 固定本次操作种类，域与后端据此选择对应的方向执行权及原生提交路径。
          r.kind = operation_kind::close;
          /// @brief 保存完整宽度的原生句柄；构造阶段不登记句柄或发起系统调用。
          r.fd = fd;
          /// @brief 按值交出完整初始化的请求参数；真正取得稳定操作槽与提交 IO 仍发生在 await_suspend。
          return r;
        }()} {}

  Close(resource_ptr resource) : Close{resource ? resource->fd() : -1} {
    /// @brief 保留共享资源租约，使挂起、取消与完成期间的句柄身份和域归属稳定。
    this->request_.resource = std::move(resource);
  }
#if defined(_WIN32)
  /** @brief 文件原生关闭通过不可取消清理责任处理，与 SOCKET 关闭器分开。 */
  Close(HANDLE handle) : Close{reinterpret_cast<native_descriptor>(handle)} {
    /// @brief 明确原生句柄种类，避免 Windows 文件 HANDLE 与 SOCKET 被错误互换。
    this->request_.native_kind = native_handle_kind::windows_handle;
    /// @brief 声明句柄生命期已由外层租约覆盖，不为同一借用句柄登记第二份拥有者。
    this->request_.bypass_resource_registration = true;
    /// @brief 标记已经接管的清理责任不可因父级停止被跳过，仍须安全执行并排空。
    this->request_.uncancellable = true;
  }
#endif

  /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
  auto await_resume() const noexcept -> expected<void> {
    /// @brief 统一完成结果为负时按实际错误域解码，同时保留请求已经完成的字节进度。
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return {};
  }
};
}
