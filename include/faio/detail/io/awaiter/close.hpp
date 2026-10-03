#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <fcntl.h>

namespace faio::io::detail {
/** @brief Close 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class Close : public IORegistrantAwaiter<Close> {
  using Base = IORegistrantAwaiter<Close>;

 public:
  Close(native_descriptor fd)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::close;
          r.fd = fd;
          return r;
        }()} {}

  Close(resource_ptr resource) : Close{resource ? resource->fd() : -1} {
    this->request_.resource = std::move(resource);
  }
#if defined(_WIN32)
  /** @brief 文件原生关闭通过不可取消清理责任处理，与 SOCKET 关闭器分开。 */
  Close(HANDLE handle) : Close{reinterpret_cast<native_descriptor>(handle)} {
    this->request_.native_kind = native_handle_kind::windows_handle;
    this->request_.bypass_resource_registration = true;
    this->request_.uncancellable = true;
  }
#endif

  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return {};
  }
};
}  // namespace faio::io::detail
