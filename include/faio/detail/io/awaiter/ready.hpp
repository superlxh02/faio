/**
 * @file faio/detail/io/awaiter/ready.hpp
 * @brief 就绪观察。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <utility>

namespace faio::io::detail {
/**  @brief 支持多个观察者的 ready awaiter，不消费数据；关闭/取消与读写遵循同一终态协议。 */
class ReadyAwaiter : public IORegistrantAwaiter<ReadyAwaiter> {
 public:
  /** @brief 构造资源就绪等待器。 */
  ReadyAwaiter(resource_ptr resource, Interest interest)
      : IORegistrantAwaiter{[&] {
          auto r = make_request(resource, operation_kind::ready);
          r.argument = static_cast<int>(interest);
          return r;
        }()} {}

  /** @brief 构造资源就绪等待器。 */
  ReadyAwaiter(native_descriptor fd, Interest interest)
      : IORegistrantAwaiter{[&] {
          io_request r;
          r.fd = fd;
          r.kind = operation_kind::ready;
          r.argument = static_cast<int>(interest);
          return r;
        }()} {}

  /** @brief 取得异步操作结果。 */
  expected<Ready> await_resume() const noexcept {
    if (_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-_user_data.result))};
    return Ready{static_cast<std::uint32_t>(_user_data.result), _user_data.transferred};
  }
};

}  // namespace faio::io::detail

namespace faio::io {
/**  @brief 操作工厂与等待器在同一头文件中定义，按需包含即可，无需引入其他 I/O 操作。 */
template <class F>
inline auto ready(F fd, Interest interest) {
  return detail::ReadyAwaiter{std::move(fd), interest};
}
}  // namespace faio::io
