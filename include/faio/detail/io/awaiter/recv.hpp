#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows_error.hpp"
#endif
#include <cstring>
#include <fcntl.h>

namespace faio::io::detail {
/** @brief Recv 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。 */
class Recv : public IORegistrantAwaiter<Recv, scalar_io_request> {
  using Base = IORegistrantAwaiter<Recv, scalar_io_request>;

 public:
  Recv(native_descriptor fd, void* buffer, std::size_t length, int flags)
      : Base{[&] {
          scalar_io_request r;  // 原位构造紧凑拥有参数，不初始化消息/地址/路径。
          r.kind = operation_kind::recv;
          r.fd = fd;
          r.buffer = buffer;
          r.length = length;
          r.flags = flags;
          return r;
        }} {}

  Recv(resource_ptr resource, void* buffer, std::size_t length, int flags)
      : Recv{resource ? resource->fd() : -1, buffer, length, flags} {
    this->request_.resource = std::move(resource);
  }

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result < 0)
#if defined(_WIN32)
      return std::unexpected{windows::make_io_error(static_cast<int>(-this->_user_data.result),
                                                    this->_user_data.transferred)};
#else
      return std::unexpected{
          Error{static_cast<int>(-this->_user_data.result), this->_user_data.transferred}};
#endif
    return static_cast<std::size_t>(this->_user_data.result);
  }
};
}  // namespace faio::io::detail
