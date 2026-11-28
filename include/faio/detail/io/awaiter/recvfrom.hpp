/**
 * @file faio/detail/io/awaiter/recvfrom.hpp
 * @brief 接收带对端地址的数据报。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif

namespace faio::io::detail {
/**
 *  @brief RecvFrom 请求；参数保存在地址稳定的 operation_state 中，
 * 挂起前不执行内核操作。
 */
class recv_from_awaiter : public io_registrant_awaiter<recv_from_awaiter> {
  using base_type =
      io_registrant_awaiter<recv_from_awaiter>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造数据报接收等待器。 */
  recv_from_awaiter(native_descriptor fd,
                    void* buffer,
                    std::size_t length,
                    int flags,
                    sockaddr* address,
                    socklen_t* address_length)
      : base_type{[&] {
          io_request r;
          r.kind = operation_kind::RECVFROM;
          r.fd = fd;
          r.buffer = buffer;
          r.length = length;
          r.flags = flags;
          r.output_address = address;
          r.output_address_length = address_length;
          return r;
        }()} {}

  /** @brief 构造数据报接收等待器。 */
  recv_from_awaiter(resource_ptr resource,
                    void* buffer,
                    std::size_t length,
                    int flags,
                    sockaddr* address,
                    socklen_t* address_length)
      : recv_from_awaiter{
            resource ? resource->fd() : -1, buffer, length, flags, address, address_length} {
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
inline auto recvfrom(F fd,
                     void* buffer,
                     std::size_t length,
                     int flags,
                     sockaddr* address,
                     socklen_t* address_length) {
  return detail::recv_from_awaiter{std::move(fd), buffer, length, flags, address, address_length};
}
}  // namespace faio::io
