/**
 * @file faio/detail/io/awaiter/shutdown.hpp
 * @brief 连接半关闭。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif

namespace faio::io::detail {
/**
 *  @brief Shutdown 请求；参数保存在地址稳定的 operation_state 中，
 * 挂起前不执行内核操作。
 */
class shutdown_awaiter : public io_registrant_awaiter<shutdown_awaiter> {
  using base_type =
      io_registrant_awaiter<shutdown_awaiter>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造 I/O 方向关闭等待器。 */
  shutdown_awaiter(native_descriptor fd, int how)
      : base_type{[&] {
          io_request r;
          r.kind = operation_kind::SHUTDOWN;
          r.fd = fd;
          r.argument = how;
          return r;
        }()} {}

  /** @brief 构造 I/O 方向关闭等待器。 */
  shutdown_awaiter(resource_ptr resource, int how)
      : shutdown_awaiter{resource ? resource->fd() : -1, how} {
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
inline auto shutdown(F fd, int how) {
  return detail::shutdown_awaiter{std::move(fd), how};
}
}  // namespace faio::io
