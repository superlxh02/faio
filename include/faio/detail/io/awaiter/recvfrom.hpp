#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <cstring>
#include <fcntl.h>
namespace faio::io::detail {
/** @brief RecvFrom 请求；参数由稳定 operation_state
 * 保存，挂起前没有内核副作用。 */
class RecvFrom : public IORegistrantAwaiter<RecvFrom> {
  using Base = IORegistrantAwaiter<RecvFrom>;

public:
  RecvFrom(int fd, void *buffer, std::size_t length, int flags,
           sockaddr *address, socklen_t *address_length)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::recvfrom;
          r.fd = fd;
          r.buffer = buffer;
          r.length = length;
          r.flags = flags;
          r.output_address = address;
          r.output_address_length = address_length;
          return r;
        }()} {}
  RecvFrom(resource_ptr resource, void *buffer, std::size_t length, int flags,
           sockaddr *address, socklen_t *address_length)
      : RecvFrom{resource ? resource->fd() : -1,
                 buffer,
                 length,
                 flags,
                 address,
                 address_length} {
    this->request_.resource = std::move(resource);
  }

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result < 0)
      return std::unexpected{Error{static_cast<int>(-this->_user_data.result),
                                   this->_user_data.transferred}};
    return static_cast<std::size_t>(this->_user_data.result);
  }
};
} // namespace faio::io::detail
