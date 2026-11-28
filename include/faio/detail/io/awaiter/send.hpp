/**
 * @file faio/detail/io/awaiter/send.hpp
 * @brief 发送数据与零拷贝发送。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif

namespace faio::io::detail {
/**  @brief Send 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class send_awaiter : public io_registrant_awaiter<send_awaiter, scalar_io_request> {
  using base_type =
      io_registrant_awaiter<send_awaiter,
                            scalar_io_request>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造套接字发送等待器。 */
  send_awaiter(native_descriptor fd, const void* buffer, std::size_t length, int flags)
      : base_type{[&] {
          scalar_io_request r;
          r.kind = operation_kind::SEND;
          r.fd = fd;
          r.const_buffer = buffer;
          r.length = length;
          r.flags = flags;
          return r;
        }} {}

  /** @brief 构造套接字发送等待器。 */
  send_awaiter(resource_ptr resource, const void* buffer, std::size_t length, int flags)
      : send_awaiter{resource ? resource->fd() : -1, buffer, length, flags} {
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

/**  @brief SendZC 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class send_zc_awaiter : public io_registrant_awaiter<send_zc_awaiter> {
  using base_type =
      io_registrant_awaiter<send_zc_awaiter>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造零拷贝发送等待器。 */
  send_zc_awaiter(
      native_descriptor fd, const void* buffer, std::size_t length, int flags, unsigned zc_flags)
      : base_type{[&] {
          io_request r;
          r.kind = operation_kind::SEND_ZC;
          r.fd = fd;
          r.const_buffer = buffer;
          r.length = length;
          r.flags = flags;
          r.argument = static_cast<int>(zc_flags);
          return r;
        }} {}

  /** @brief 构造零拷贝发送等待器。 */
  send_zc_awaiter(resource_ptr resource,
                  const void* buffer,
                  std::size_t length,
                  int flags,
                  unsigned zc_flags = 0)
      : send_zc_awaiter{resource ? resource->fd() : -1, buffer, length, flags, zc_flags} {
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
inline auto send(F fd, const void* buffer, std::size_t length, int flags = 0) {
  return detail::send_awaiter{std::move(fd), buffer, length, flags};
}

/** @brief 提交零拷贝发送请求。 */
template <class F>
inline auto send_zc(
    F fd, const void* buffer, std::size_t length, int flags, unsigned zc_flags = 0) {
  return detail::send_zc_awaiter{std::move(fd), buffer, length, flags, zc_flags};
}
}  // namespace faio::io
