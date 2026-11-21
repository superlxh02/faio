/**
 * @file faio/detail/io/awaiter/sendto.hpp
 * @brief 向对端发送数据报。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif
#include <cstring>

namespace faio::io::detail {
/**  @brief SendTo 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class SendTo : public IORegistrantAwaiter<SendTo> {
  using Base = IORegistrantAwaiter<SendTo>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造数据报发送等待器。 */
  SendTo(native_descriptor fd,
         const void* buffer,
         std::size_t length,
         int flags,
         const sockaddr* address,
         socklen_t address_length)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::sendto;
          r.fd = fd;
          r.const_buffer = buffer;
          r.length = length;
          r.flags = flags;
          r.address_length = address_length;
          if (static_cast<std::size_t>(address_length) <= sizeof(r.address) && address)
            std::memcpy(&r.address, address, address_length);
          else
            r.kind = operation_kind::unsupported;
          return r;
        }()} {}

  /** @brief 构造数据报发送等待器。 */
  SendTo(resource_ptr resource,
         const void* buffer,
         std::size_t length,
         int flags,
         const sockaddr* address,
         socklen_t address_length)
      : SendTo{resource ? resource->fd() : -1, buffer, length, flags, address, address_length} {
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
inline auto sendto(F fd,
                   const void* buffer,
                   std::size_t length,
                   int flags,
                   const sockaddr* address,
                   socklen_t address_length) {
  return detail::SendTo{std::move(fd), buffer, length, flags, address, address_length};
}
}  // namespace faio::io
