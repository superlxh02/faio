#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows_error.hpp"
#endif
#include <cstring>
#include <fcntl.h>

namespace faio::io::detail {
/** @brief RecvMsg 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class RecvMsg : public IORegistrantAwaiter<RecvMsg> {
  using Base = IORegistrantAwaiter<RecvMsg>;

 public:
  RecvMsg(native_descriptor fd, msghdr* message, unsigned flags)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::recvmsg;
          r.fd = fd;
          r.output_message = message;
          r.flags = static_cast<int>(flags);
          return r;
        }()} {}

  RecvMsg(resource_ptr resource, msghdr* message, unsigned flags)
      : RecvMsg{resource ? resource->fd() : -1, message, flags} {
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
