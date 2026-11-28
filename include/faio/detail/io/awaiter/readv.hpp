/**
 * @file faio/detail/io/awaiter/readv.hpp
 * @brief 分散读取。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>

namespace faio::io::detail {
/**  @brief ReadV 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class read_v_awaiter : public io_registrant_awaiter<read_v_awaiter> {
  using base_type =
      io_registrant_awaiter<read_v_awaiter>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造向量读取等待器。 */
  read_v_awaiter(native_descriptor fd,
                 const iovec* vectors,
                 unsigned count,
                 std::uint64_t offset,
                 int flags = 0)
      : base_type{[&] {
          io_request r;
          r.kind = operation_kind::READV;
#if defined(_WIN32)
          r.native_kind = native_handle_kind::WINDOWS_HANDLE;
          r.bypass_resource_registration = true;
          r.windows_implicit_cursor = offset == UINT64_MAX;
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

  /** @brief 构造向量读取等待器。 */
  read_v_awaiter(resource_ptr resource,
                 const iovec* vectors,
                 unsigned count,
                 std::uint64_t offset,
                 int flags = 0)
      : read_v_awaiter{resource ? resource->fd() : -1, vectors, count, offset, flags} {
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
inline auto readv(
    F fd, const iovec* vectors, unsigned count, std::uint64_t offset = UINT64_MAX, int flags = 0) {
  return detail::read_v_awaiter{std::move(fd), vectors, count, offset, flags};
}
}  // namespace faio::io
