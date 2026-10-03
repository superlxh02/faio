#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <cstring>
#include <fcntl.h>

namespace faio::io::detail {
/** @brief ReadV 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。
 */
class ReadV : public IORegistrantAwaiter<ReadV> {
  using Base = IORegistrantAwaiter<ReadV>;

 public:
  ReadV(native_descriptor fd,
        const iovec* vectors,
        unsigned count,
        std::uint64_t offset,
        int flags = 0)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::readv;
#if defined(_WIN32)
          r.native_kind = native_handle_kind::windows_handle;
          r.bypass_resource_registration = true;
          r.windows_implicit_cursor =
              offset == UINT64_MAX;  // 整个 vectored 请求保留同一 OS 游标锁。
#endif
          r.fd = fd;
          if (count > native_iov_limit || count > INT_MAX)
            r.validation_error = EINVAL;
          else if (count && !vectors)
            r.validation_error = EFAULT;
          else if (count)
            r.vectors.assign(vectors, vectors + count);
          r.offset = offset;
          r.flags = flags;
          return r;
        }()} {
  }

  ReadV(resource_ptr resource,
        const iovec* vectors,
        unsigned count,
        std::uint64_t offset,
        int flags = 0)
      : ReadV{resource ? resource->fd() : -1, vectors, count, offset, flags} {
    this->request_.resource = std::move(resource);
  }

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return static_cast<std::size_t>(this->_user_data.result);
  }
};
}  // namespace faio::io::detail
