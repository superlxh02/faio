/**
 * @file faio/detail/io/awaiter/cancel.hpp
 * @brief 按代际取消操作。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>

namespace faio::io::detail {
/**
 * @brief Cancel 请求；0、ALL(1)、FD(2) 和 ALL|FD(3) 均遵循本资源的统一取消语义。
 * @details 控制请求成功不表示原 CQE 已消费；原请求 await_resume 后才可复用数据缓冲区。
 * FD_FIXED、ANY 等需要额外原生身份的匹配选项返回 ENOTSUP。
 * 参数保存在地址稳定的 operation_state 中，挂起前不执行内核操作。
 */
class Cancel : public IORegistrantAwaiter<Cancel> {
  using Base = IORegistrantAwaiter<Cancel>; /**< 共用请求注册等待器的提交、取消和完成握手。 */

 public:
  /** @brief 构造 I/O 取消等待器。 */
  Cancel(native_descriptor fd, unsigned flags)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::cancel;
          r.fd = fd;
          r.flags = static_cast<int>(flags);
          return r;
        }()} {}

  /** @brief 构造 I/O 取消等待器。 */
  Cancel(resource_ptr resource, unsigned flags) : Cancel{resource ? resource->fd() : -1, flags} {
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
inline auto cancel(F fd, unsigned flags = 0) {
  return detail::Cancel{std::move(fd), flags};
}
}  // namespace faio::io
