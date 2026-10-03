#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <cstring>
#include <fcntl.h>
namespace faio::io::detail {
/** @brief Accept 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class Accept : public IORegistrantAwaiter<Accept> {
  using Base = IORegistrantAwaiter<Accept>;

public:
  Accept(int fd, sockaddr *address, socklen_t *length, int flags)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::accept;
          r.fd = fd;
          r.output_address = address;
          r.output_address_length = length;
          r.flags = flags;
          return r;
        }()} {}
  Accept(resource_ptr resource, sockaddr *address, socklen_t *length, int flags)
      : Accept{resource ? resource->fd() : -1, address, length, flags} {
    this->request_.resource = std::move(resource);
  }

  auto await_resume() const noexcept -> expected<int> {
    if (this->_user_data.result < 0)
      return std::unexpected{Error{static_cast<int>(-this->_user_data.result),
                                   this->_user_data.transferred}};
    return static_cast<int>(this->_user_data.result);
  }
  /** @brief 原生即刻接受：无待接连接时 CQE 返回 EAGAIN，不等待下一连接。 */
  auto no_wait() & noexcept -> Accept & {
    this->request_.kind = operation_kind::accept_nowait;
    return *this;
  }
  auto no_wait() && noexcept -> Accept {
    this->request_.kind = operation_kind::accept_nowait;
    return std::move(*this);
  }
};
} // namespace faio::io::detail
