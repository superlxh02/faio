#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <cstring>
#include <fcntl.h>
namespace faio::io::detail {
/** @brief Write 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class Write : public IORegistrantAwaiter<Write> {
  using Base = IORegistrantAwaiter<Write>;

public:
  Write(int fd, const void *buffer, std::size_t length, std::uint64_t offset)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::write;
          r.fd = fd;
          r.const_buffer = buffer;
          r.length = length;
          r.offset = offset;
          return r;
        }()} {}
  Write(resource_ptr resource, const void *buffer, std::size_t length,
        std::uint64_t offset)
      : Write{resource ? resource->fd() : -1, buffer, length, offset} {
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
