/**
 * @file faio/detail/io/awaiter/write.hpp
 * @brief 写入字节。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>

namespace faio::io::detail {
/**  @brief Write 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class Write : public IORegistrantAwaiter<Write> {
  using Base = IORegistrantAwaiter<Write>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造写入等待器。 */
  Write(native_descriptor fd, const void* buffer, std::size_t length, std::uint64_t offset)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::write;
#if defined(_WIN32)
          r.native_kind = native_handle_kind::windows_handle;
          r.bypass_resource_registration = true;
          r.windows_implicit_cursor = offset == UINT64_MAX;
#endif
          r.fd = fd;
          r.const_buffer = buffer;
          r.length = length;
          r.offset = offset;
          return r;
        }()} {
  }

  /** @brief 构造写入等待器。 */
  Write(resource_ptr resource, const void* buffer, std::size_t length, std::uint64_t offset)
      : Write{resource ? resource->fd() : -1, buffer, length, offset} {
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
inline auto write(F fd, const void* buffer, std::size_t length, std::uint64_t offset = UINT64_MAX) {
  return detail::Write{std::move(fd), buffer, length, offset};
}
}  // namespace faio::io
