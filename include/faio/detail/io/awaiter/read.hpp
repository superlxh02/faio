/**
 * @file faio/detail/io/awaiter/read.hpp
 * @brief 读取字节。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>

namespace faio::io::detail {
/**  @brief Read 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class Read : public IORegistrantAwaiter<Read> {
  using Base = IORegistrantAwaiter<Read>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造读取等待器。 */
  Read(native_descriptor fd, void* buffer, std::size_t length, std::uint64_t offset)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::read;
#if defined(_WIN32)
          r.native_kind = native_handle_kind::windows_handle;
          r.bypass_resource_registration = true;
          r.windows_implicit_cursor = offset == UINT64_MAX;
#endif
          r.fd = fd;
          r.buffer = buffer;
          r.length = length;
          r.offset = offset;
          return r;
        }()} {
  }

  /** @brief 构造读取等待器。 */
  Read(resource_ptr resource, void* buffer, std::size_t length, std::uint64_t offset)
      : Read{resource ? resource->fd() : -1, buffer, length, offset} {
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
inline auto read(F fd, void* buffer, std::size_t length, std::uint64_t offset = UINT64_MAX) {
  return detail::Read{std::move(fd), buffer, length, offset};
}
}  // namespace faio::io
