/**
 * @file faio/detail/io/awaiter/socket.hpp
 * @brief 套接字资源封装。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif

namespace faio::io::detail {
/**  @brief Socket 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class socket : public io_registrant_awaiter<socket> {
  using base_type =
      io_registrant_awaiter<socket>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造套接字创建等待器。 */
  socket(int domain, int type, int protocol, unsigned flags)
      : base_type{[&] {
          io_request r;
          r.kind = operation_kind::SOCKET;
          r.argument = domain;
          r.argument2 = type;
          r.argument3 = protocol;
          r.flags = static_cast<int>(flags);
          return r;
        }()} {}

  /** @brief 取得异步操作结果。 */
  auto await_resume() const noexcept -> expected<native_descriptor> {
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return static_cast<native_descriptor>(this->_user_data.result);
  }
};
}  // namespace faio::io::detail

namespace faio::io {
/**  @brief 操作工厂与等待器在同一头文件中定义，按需包含即可，无需引入其他 I/O 操作。 */
inline auto socket(int family, int type, int protocol, unsigned flags = 0) {
  return detail::socket{family, type, protocol, flags};
}
}  // namespace faio::io
