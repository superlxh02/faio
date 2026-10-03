#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <concepts>
#include <type_traits>
#include <utility>
namespace faio::time::detail {
/** @brief 兼容超时拥有包装；deadline 已在 request 中，后端不借用 timer 指针。
 * @tparam T 公开继承某个完整或紧凑 IORegistrantAwaiter 的无 cv/ref 操作类型。
 * @details 包装按值移动拥有 operation，右值源对象可在真实挂起后销毁。
 *          约束读取公共基类别名，不假定 Request
 * 策略；本头直接包含唯一模板定义。
 */
template <class T>
  requires io::detail::io_registrant_operation<T> &&
           std::same_as<T, std::remove_cvref_t<T>>
class Timeout : public T {
public:
  explicit Timeout(T &&operation) noexcept(
      std::is_nothrow_move_constructible_v<T>)
      : T(std::move(operation)) {}
};
} // namespace faio::time::detail
