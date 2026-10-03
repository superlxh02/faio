#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <cstring>
#include <fcntl.h>

namespace faio::io::detail {
/** @brief Fsync 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class Fsync : public IORegistrantAwaiter<Fsync> {
  using Base = IORegistrantAwaiter<Fsync>;

 public:
  Fsync(native_descriptor fd, unsigned flags)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::fsync;
#if defined(_WIN32)
          r.native_kind = native_handle_kind::windows_handle;
          r.bypass_resource_registration = true;
#endif
          r.fd = fd;
          r.flags = static_cast<int>(flags);
          return r;
        }()} {
  }

  Fsync(resource_ptr resource, unsigned flags) : Fsync{resource ? resource->fd() : -1, flags} {
    this->request_.resource = std::move(resource);
  }

  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return {};
  }
};
}  // namespace faio::io::detail
