/**
 * @file faio/detail/io/awaiter/accept.hpp
 * @brief 接受连接。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif

namespace faio::io::detail {
/**  @brief Accept 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class accept_awaiter : public io_registrant_awaiter<accept_awaiter> {
  using base_type =
      io_registrant_awaiter<accept_awaiter>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造接受连接的等待器。 */
  accept_awaiter(native_descriptor fd, sockaddr* address, socklen_t* length, int flags)
      : base_type{[&] {
          io_request r;
          r.kind = operation_kind::ACCEPT;
          r.fd = fd;
          r.output_address = address;
          r.output_address_length = length;
          r.flags = flags;
          return r;
        }()} {}

  /** @brief 构造接受连接的等待器。 */
  accept_awaiter(resource_ptr resource, sockaddr* address, socklen_t* length, int flags)
      : accept_awaiter{resource ? resource->fd() : -1, address, length, flags} {
    this->request_.resource = std::move(resource);
  }

  /** @brief 取得异步操作结果。 */
  auto await_resume() const noexcept -> expected<native_descriptor> {
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return static_cast<native_descriptor>(this->_user_data.result);
  }

  /**  @brief 将请求设为立即接受连接；没有待接连接时，CQE 返回 EAGAIN，不等待下一条连接。 */
  auto no_wait() & noexcept -> accept_awaiter& {
    this->request_.kind = operation_kind::ACCEPT_NOWAIT;
    return *this;
  }

  /** @brief 将请求设为立即接受连接，不等待后续连接到达。 */
  auto no_wait() && noexcept -> accept_awaiter {
    this->request_.kind = operation_kind::ACCEPT_NOWAIT;
    return std::move(*this);
  }
};
}  // namespace faio::io::detail

namespace faio::io {
/**  @brief 操作工厂与等待器在同一头文件中定义，按需包含即可，无需引入其他 I/O 操作。 */
template <class F>
inline auto accept(F fd, sockaddr* address, socklen_t* length, int flags = 0) {
  return detail::accept_awaiter{std::move(fd), address, length, flags};
}
}  // namespace faio::io
