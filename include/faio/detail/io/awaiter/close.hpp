#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <fcntl.h>
namespace faio::io::detail {
/** @brief Close 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class Close : public IORegistrantAwaiter<Close> {
  using Base = IORegistrantAwaiter<Close>;

public:
  Close(int fd)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::close;
          r.fd = fd;
          return r;
        }()} {}
  Close(resource_ptr resource) : Close{resource ? resource->fd() : -1} {
    this->request_.resource = std::move(resource);
  }

  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result < 0)
      return std::unexpected{Error{static_cast<int>(-this->_user_data.result),
                                   this->_user_data.transferred}};
    return {};
  }
};
} // namespace faio::io::detail
