/**
 * @file faio/detail/time/timeout.hpp
 * @brief 超时适配器、超时竞争与安全排空。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <concepts>
#include <type_traits>
#include <utility>

namespace faio::time::detail {
/**
 *  @brief 持有操作对象的超时包装，兼容既有接口；deadline 保存在 request 中，后端不借用 timer 指针。
 * @tparam T 公开继承完整或紧凑 io_registrant_awaiter 的操作类型，不含 const、volatile 或引用限定。
 * @details 通过移动按值持有 operation，右值源对象可在实际挂起后销毁。
 *          通过公共基类别名检查类型约束，不预设 Request 策略；本头文件直接包含该模板的唯一定义。
 */
template <class T>
  requires io::detail::io_registrant_operation<T> && std::same_as<T, std::remove_cvref_t<T>>
class timeout_adapter : public T {
 public:
  /** @brief 构造异步超时适配器。 */
  explicit timeout_adapter(T&& operation) noexcept(std::is_nothrow_move_constructible_v<T>)
      : T(std::move(operation)) {}
};
}  // namespace faio::time::detail
