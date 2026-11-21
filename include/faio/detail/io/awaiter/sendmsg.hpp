/**
 * @file faio/detail/io/awaiter/sendmsg.hpp
 * @brief 发送消息与附属数据。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif

namespace faio::io::detail {
/**  @brief SendMsg 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class SendMsg : public IORegistrantAwaiter<SendMsg> {
  using Base = IORegistrantAwaiter<SendMsg>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造消息发送等待器。 */
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

  /** @brief 构造消息发送等待器。 */
  SendMsg(resource_ptr resource, const msghdr* message, unsigned flags)
      : SendMsg{resource ? resource->fd() : -1, message, flags} {
    this->request_.resource = std::move(resource);
  }

  /** @brief 取得异步操作结果。 */
  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return static_cast<std::size_t>(this->_user_data.result);
  }
};

/**
 *  @brief SendMsgZC 请求；参数保存在地址稳定的 operation_state 中，
 * 挂起前不执行内核操作。
 */
class SendMsgZC : public IORegistrantAwaiter<SendMsgZC> {
  using Base = IORegistrantAwaiter<SendMsgZC>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造消息零拷贝发送等待器。 */
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

  /** @brief 构造消息零拷贝发送等待器。 */
  SendMsgZC(resource_ptr resource, const msghdr* message, unsigned flags)
      : SendMsgZC{resource ? resource->fd() : -1, message, flags} {
    request_.resource = std::move(resource);
  }

  /** @brief 取得异步操作结果。 */
  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return static_cast<std::size_t>(this->_user_data.result);
  }
};
}  // namespace faio::io::detail

namespace faio::io {
/**  @brief 操作工厂与等待器在同一头文件中定义，按需包含即可，无需引入其他 I/O 操作。 */
template <class F>
inline auto sendmsg(F fd, const msghdr* message, unsigned flags = 0) {
  return detail::SendMsg{std::move(fd), message, flags};
}

/** @brief 提交向量零拷贝发送请求。 */
template <class F>
inline auto sendmsg_zc(F fd, const msghdr* message, unsigned flags = 0) {
  return detail::SendMsgZC{std::move(fd), message, flags};
}
}  // namespace faio::io
