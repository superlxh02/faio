#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <cstring>
#include <fcntl.h>
namespace faio::io::detail {
/** @brief Connect 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class Connect : public IORegistrantAwaiter<Connect> {
  using Base = IORegistrantAwaiter<Connect>;

public:
  Connect(int fd, const sockaddr *address, socklen_t length)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::connect;
          r.fd = fd;
          r.address_length = length;
          if (length <= sizeof(r.address) && address)
            std::memcpy(&r.address, address, length);
          else
            r.kind = operation_kind::unsupported;
          return r;
        }()} {}
  Connect(resource_ptr resource, const sockaddr *address, socklen_t length)
      : Connect{resource ? resource->fd() : -1, address, length} {
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
