/**
 * @file faio/detail/common/cancellation.hpp
 * @brief 取消异常及其处理策略。
 */
#ifndef FAIO_DETAIL_COMMON_CANCELLATION_HPP
#define FAIO_DETAIL_COMMON_CANCELLATION_HPP
#include <exception>

namespace faio {
/** @brief 等待操作响应 stop 请求时抛出；协程可显式捕获此异常并完成清理。 */
struct operation_cancelled : std::exception {
  /** @brief 取得异常说明。 */
  const char* what() const noexcept override { return "协程等待操作已取消"; }
};
}  // namespace faio
#endif
