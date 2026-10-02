#ifndef FAIO_DETAIL_COMMON_CANCELLATION_HPP
#define FAIO_DETAIL_COMMON_CANCELLATION_HPP
#include <exception>
namespace faio {
// 等待操作接受 stop 请求后抛出；协程代码可显式捕获并完成自己的清理。
struct operation_cancelled : std::exception {
  const char* what() const noexcept override { return "协程等待操作已取消"; }
};
} // namespace faio
#endif
