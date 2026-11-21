/**
 * @file faio/detail/io/awaiter/fsync.hpp
 * @brief 文件持久化。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>

namespace faio::io::detail {
/**  @brief Fsync 请求；参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。 */
class Fsync : public IORegistrantAwaiter<Fsync> {
  using Base = IORegistrantAwaiter<Fsync>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造文件持久化等待器。 */
  Fsync(native_descriptor fd, unsigned flags)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::fsync;
#if defined(_WIN32)
          r.native_kind = native_handle_kind::windows_handle;
          r.bypass_resource_registration = true;
#endif
          r.fd = fd;
          r.flags = static_cast<int>(flags);
          return r;
        }()} {
  }

  /** @brief 构造文件持久化等待器。 */
  Fsync(resource_ptr resource, unsigned flags) : Fsync{resource ? resource->fd() : -1, flags} {
    this->request_.resource = std::move(resource);
  }

  /** @brief 取得异步操作结果。 */
  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-this->_user_data.result),
                                             this->_user_data.transferred)};
    return {};
  }
};
}  // namespace faio::io::detail

namespace faio::io {
/**  @brief 操作工厂与等待器在同一头文件中定义，按需包含即可，无需引入其他 I/O 操作。 */
template <class F>
inline auto fsync(F fd, unsigned flags = 0) {
  return detail::Fsync{std::move(fd), flags};
}
}  // namespace faio::io
