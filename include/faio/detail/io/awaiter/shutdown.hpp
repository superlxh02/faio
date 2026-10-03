#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <cstring>
#include <fcntl.h>
namespace faio::io::detail {
/** @brief Shutdown 请求；参数由稳定 operation_state
 * 保存，挂起前没有内核副作用。 */
class Shutdown : public IORegistrantAwaiter<Shutdown> {
  using Base = IORegistrantAwaiter<Shutdown>;

public:
  Shutdown(int fd, int how)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::shutdown;
          r.fd = fd;
          r.argument = how;
          return r;
        }()} {}
  Shutdown(resource_ptr resource, int how)
      : Shutdown{resource ? resource->fd() : -1, how} {
    this->request_.resource = std::move(resource);
  }

  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result < 0)
      return std::unexpected{Error{static_cast<int>(-this->_user_data.result),
                                   this->_user_data.transferred}};
    return {};
  }
};
} // namespace faio::io::detail
