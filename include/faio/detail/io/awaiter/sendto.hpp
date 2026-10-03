#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <cstring>
#include <fcntl.h>
namespace faio::io::detail {
/** @brief SendTo 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class SendTo : public IORegistrantAwaiter<SendTo> {
  using Base = IORegistrantAwaiter<SendTo>;

public:
  SendTo(int fd, const void *buffer, std::size_t length, int flags,
         const sockaddr *address, socklen_t address_length)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::sendto;
          r.fd = fd;
          r.const_buffer = buffer;
          r.length = length;
          r.flags = flags;
          r.address_length = address_length;
          if (address_length <= sizeof(r.address) && address)
            std::memcpy(&r.address, address, address_length);
          else
            r.kind = operation_kind::unsupported;
          return r;
        }()} {}
  SendTo(resource_ptr resource, const void *buffer, std::size_t length,
         int flags, const sockaddr *address, socklen_t address_length)
      : SendTo{resource ? resource->fd() : -1,
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
