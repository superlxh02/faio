#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <cstring>
#include <fcntl.h>
namespace faio::io::detail {
/** @brief Cancel 请求；0/ALL(1)/FD(2)/ALL|FD(3) 兼容本资源的统一取消语义。
 * @details 控制成功不表示 original CQE 已消费；数据缓冲区必须等原请求
 * await_resume 后才可复用。 FD_FIXED/ANY 等需要额外原生身份的匹配选项明确返回
 * ENOTSUP。 参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class Cancel : public IORegistrantAwaiter<Cancel> {
  using Base = IORegistrantAwaiter<Cancel>;

public:
  Cancel(int fd, unsigned flags)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::cancel;
          r.fd = fd;
          r.flags = static_cast<int>(flags);
          return r;
        }()} {}
  Cancel(resource_ptr resource, unsigned flags)
      : Cancel{resource ? resource->fd() : -1, flags} {
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
