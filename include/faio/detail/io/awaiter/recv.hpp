/**
 * @file faio/detail/io/awaiter/recv.hpp
 * @brief 接收数据。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif

namespace faio::io::detail {
/**  @brief Recv 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class Recv : public IORegistrantAwaiter<Recv, scalar_io_request> {
  using Base =
      IORegistrantAwaiter<Recv,
                          scalar_io_request>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造套接字接收等待器。 */
  Recv(native_descriptor fd, void* buffer, std::size_t length, int flags)
      : Base{[&] {
          scalar_io_request r;
          r.kind = operation_kind::recv;
          r.fd = fd;
          r.buffer = buffer;
          r.length = length;
          r.flags = flags;
          return r;
        }} {}

  /** @brief 构造套接字接收等待器。 */
  Recv(resource_ptr resource, void* buffer, std::size_t length, int flags)
      : Recv{resource ? resource->fd() : -1, buffer, length, flags} {
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
inline auto recv(F fd, void* buffer, std::size_t length, int flags = 0) {
  return detail::Recv{std::move(fd), buffer, length, flags};
}
}  // namespace faio::io
