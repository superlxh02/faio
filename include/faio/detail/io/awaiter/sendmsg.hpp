#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows_error.hpp"
#endif
#include <cstring>
#include <fcntl.h>

namespace faio::io::detail {
/** @brief SendMsg 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class SendMsg : public IORegistrantAwaiter<SendMsg> {
  using Base = IORegistrantAwaiter<SendMsg>;

 public:
  SendMsg(native_descriptor fd, const msghdr* message, unsigned flags)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::sendmsg;
          r.fd = fd;
          r.flags = static_cast<int>(flags);
          if (!message) {
            r.validation_error = EFAULT;
            return r;
          }
          r.message = *message;
          if (static_cast<std::uint64_t>(message->msg_iovlen) > native_iov_limit) {
            r.validation_error = EINVAL;
            return r;
          }
          if (message->msg_iovlen && !message->msg_iov) {
            r.validation_error = EFAULT;
            return r;
          }
          if (message->msg_iovlen)
            r.vectors.assign(message->msg_iov, message->msg_iov + message->msg_iovlen);
          return r;
        }()} {}

  SendMsg(resource_ptr resource, const msghdr* message, unsigned flags)
      : SendMsg{resource ? resource->fd() : -1, message, flags} {
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

/** @brief SendMsgZC 请求；参数由稳定 operation_state
 * 保存，挂起前没有内核副作用。 */
class SendMsgZC : public IORegistrantAwaiter<SendMsgZC> {
  using Base = IORegistrantAwaiter<SendMsgZC>;

 public:
  SendMsgZC(native_descriptor fd, const msghdr* message, unsigned flags)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::sendmsg_zc;
          r.fd = fd;
          r.flags = static_cast<int>(flags);
          if (!message) {
            r.validation_error = EFAULT;
            return r;
          }
          r.message = *message;
          if (static_cast<std::uint64_t>(message->msg_iovlen) > native_iov_limit) {
            r.validation_error = EINVAL;
            return r;
          }
          if (message->msg_iovlen && !message->msg_iov) {
            r.validation_error = EFAULT;
            return r;
          }
          if (message->msg_iovlen)
            r.vectors.assign(message->msg_iov, message->msg_iov + message->msg_iovlen);
          return r;
        }()} {}

  SendMsgZC(resource_ptr resource, const msghdr* message, unsigned flags)
      : SendMsgZC{resource ? resource->fd() : -1, message, flags} {
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
