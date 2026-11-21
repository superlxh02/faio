/**
 * @file faio/detail/io/awaiter/close.hpp
 * @brief 安全关闭。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>

namespace faio::io::detail {
/**  @brief Close 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class Close : public IORegistrantAwaiter<Close> {
  using Base = IORegistrantAwaiter<Close>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造资源关闭等待器。 */
  Close(native_descriptor fd)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::close;
          r.fd = fd;
          return r;
        }()} {}

  /** @brief 构造资源关闭等待器。 */
  Close(resource_ptr resource) : Close{resource ? resource->fd() : -1} {
    this->request_.resource = std::move(resource);
  }
#if defined(_WIN32)
  /**  @brief 原生文件关闭由不可取消的清理流程处理，与 SOCKET 的关闭流程分开。 */
  Close(HANDLE handle) : Close{reinterpret_cast<native_descriptor>(handle)} {
    this->request_.native_kind = native_handle_kind::windows_handle;
    this->request_.bypass_resource_registration = true;
    this->request_.uncancellable = true;
  }
#endif

  /** @brief 取得异步操作结果。 */
  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return {};
  }
};
}  // namespace faio::io::detail

namespace faio::io {
/**  @brief 操作工厂与等待器在同一头文件中定义，按需包含即可，无需引入其他 I/O 操作。 */
template <class F>
inline auto close(F fd) {
  return detail::Close{std::move(fd)};
}
}  // namespace faio::io
