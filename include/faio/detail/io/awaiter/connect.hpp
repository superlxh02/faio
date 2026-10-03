#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows_error.hpp"
#endif
#include <cstring>
#include <fcntl.h>

namespace faio::io::detail {
/** @brief Connect 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class Connect : public IORegistrantAwaiter<Connect> {
  using Base = IORegistrantAwaiter<Connect>;

 public:
  Connect(native_descriptor fd, const sockaddr* address, socklen_t length)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::connect;
          r.fd = fd;
          r.address_length = length;
          // 显式扩大为 size_t；Windows 负 socklen_t 转换后超容量，不发生越界复制。
          if (static_cast<std::size_t>(length) <= sizeof(r.address) && address)
            std::memcpy(&r.address, address, length);
          else
            r.kind = operation_kind::unsupported;
          return r;
        }()} {}

  Connect(resource_ptr resource, const sockaddr* address, socklen_t length)
      : Connect{resource ? resource->fd() : -1, address, length} {
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
