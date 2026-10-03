#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows_error.hpp"
#endif
#include <cstring>
#include <fcntl.h>

namespace faio::io::detail {
/** @brief Send 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。 */
class Send : public IORegistrantAwaiter<Send, scalar_io_request> {
  using Base = IORegistrantAwaiter<Send, scalar_io_request>;

 public:
  Send(native_descriptor fd, const void* buffer, std::size_t length, int flags)
      : Base{[&] {
          scalar_io_request r;  // 原位构造紧凑拥有参数，不初始化消息/地址/路径。
          r.kind = operation_kind::send;
          r.fd = fd;
          r.const_buffer = buffer;
          r.length = length;
          r.flags = flags;
          return r;
        }} {}

  Send(resource_ptr resource, const void* buffer, std::size_t length, int flags)
      : Send{resource ? resource->fd() : -1, buffer, length, flags} {
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

/** @brief SendZC 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class SendZC : public IORegistrantAwaiter<SendZC> {
  using Base = IORegistrantAwaiter<SendZC>;

 public:
  SendZC(native_descriptor fd, const void* buffer, std::size_t length, int flags, unsigned zc_flags)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::send_zc;
          r.fd = fd;
          r.const_buffer = buffer;
          r.length = length;
          r.flags = flags;
          r.argument = static_cast<int>(zc_flags);
          return r;
        }} {}

  SendZC(resource_ptr resource,
         const void* buffer,
         std::size_t length,
         int flags,
         unsigned zc_flags = 0)
      : SendZC{resource ? resource->fd() : -1, buffer, length, flags, zc_flags} {
    request_.resource = std::move(resource);
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
