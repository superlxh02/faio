#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <cstring>
#include <fcntl.h>
namespace faio::io::detail {
/** @brief Read 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。 */
class Read : public IORegistrantAwaiter<Read> {
  using Base = IORegistrantAwaiter<Read>;

public:
  Read(int fd, void *buffer, std::size_t length, std::uint64_t offset)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::read;
          r.fd = fd;
          r.buffer = buffer;
          r.length = length;
          r.offset = offset;
          return r;
        }()} {}
  Read(resource_ptr resource, void *buffer, std::size_t length,
       std::uint64_t offset)
      : Read{resource ? resource->fd() : -1, buffer, length, offset} {
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
