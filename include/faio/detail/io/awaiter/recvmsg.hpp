/**
 * @file faio/detail/io/awaiter/recvmsg.hpp
 * @brief 接收消息与附属数据。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif

namespace faio::io::detail {
/**  @brief RecvMsg 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class recv_msg_awaiter : public io_registrant_awaiter<recv_msg_awaiter> {
  using base_type =
      io_registrant_awaiter<recv_msg_awaiter>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造消息接收等待器。 */
  recv_msg_awaiter(native_descriptor fd, msghdr* message, unsigned flags)
      : base_type{[&] {
          io_request r;
          r.kind = operation_kind::RECVMSG;
          r.fd = fd;
          r.output_message = message;
          r.flags = static_cast<int>(flags);
          return r;
        }()} {}

  /** @brief 构造消息接收等待器。 */
  recv_msg_awaiter(resource_ptr resource, msghdr* message, unsigned flags)
      : recv_msg_awaiter{resource ? resource->fd() : -1, message, flags} {
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
}  // namespace faio::io::detail

namespace faio::io {
/**  @brief 操作工厂与等待器在同一头文件中定义，按需包含即可，无需引入其他 I/O 操作。 */
template <class F>
inline auto recvmsg(F fd, msghdr* message, unsigned flags = 0) {
  return detail::recv_msg_awaiter{std::move(fd), message, flags};
}
}  // namespace faio::io
