/**
 * @file faio/detail/io/awaiter/connect.hpp
 * @brief 建立连接。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif
#include <cstring>

namespace faio::io::detail {
/**  @brief Connect 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class connect_awaiter : public io_registrant_awaiter<connect_awaiter> {
  using base_type =
      io_registrant_awaiter<connect_awaiter>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造建立连接的等待器。 */
  connect_awaiter(native_descriptor fd, const sockaddr* address, socklen_t length)
      : base_type{[&] {
          io_request r;
          r.kind = operation_kind::CONNECT;
          r.fd = fd;
          r.address_length = length;
          if (static_cast<std::size_t>(length) <= sizeof(r.address) && address)
            std::memcpy(&r.address, address, length);
          else
            r.kind = operation_kind::UNSUPPORTED;
          return r;
        }()} {}

  /** @brief 构造建立连接的等待器。 */
  connect_awaiter(resource_ptr resource, const sockaddr* address, socklen_t length)
      : connect_awaiter{resource ? resource->fd() : -1, address, length} {
    this->request_.resource = std::move(resource);
  }

  /** @brief 取得异步操作结果。 */
  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return {};
  }
};
}  // namespace faio::io::detail

namespace faio::io {
/**  @brief 操作工厂与等待器在同一头文件中定义，按需包含即可，无需引入其他 I/O 操作。 */
template <class F>
inline auto connect(F fd, const sockaddr* address, socklen_t length) {
  return detail::connect_awaiter{std::move(fd), address, length};
}
}  // namespace faio::io
