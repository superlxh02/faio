#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows_error.hpp"
#endif
#include <cstring>
#include <fcntl.h>

namespace faio::io::detail {
/** @brief Shutdown 请求；参数由稳定 operation_state
 * 保存，挂起前没有内核副作用。 */
class Shutdown : public IORegistrantAwaiter<Shutdown> {
  using Base = IORegistrantAwaiter<Shutdown>;

 public:
  Shutdown(native_descriptor fd, int how)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::shutdown;
          r.fd = fd;
          r.argument = how;
          return r;
        }()} {}

  Shutdown(resource_ptr resource, int how) : Shutdown{resource ? resource->fd() : -1, how} {
    this->request_.resource = std::move(resource);
  }

  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result < 0)
#if defined(_WIN32)
      return std::unexpected{windows::make_io_error(static_cast<int>(-this->_user_data.result),
                                                    this->_user_data.transferred)};
#else
      return std::unexpected{
          Error{static_cast<int>(-this->_user_data.result), this->_user_data.transferred}};
#endif
    return {};
  }
};
}  // namespace faio::io::detail
