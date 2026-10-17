/**
 * @file faio/detail/io/traits/awaitable.hpp
 * @brief 跨平台异步 IO：等待器概念。
 * @details 以编译期约束检查接口形状及返回类型；概念检查本身不执行 IO，也不承担资源生命周期。
 */
#pragma once
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/base/buffer.hpp"
#include <concepts>
#include <coroutine>
#include <type_traits>
#include <utility>

namespace faio::io {
namespace detail {
/** @brief 解析成员/ADL co_await 或直接 awaiter，用于实际结果类型检查。 */
template <class T>
decltype(auto) get_awaiter(T&& value) {
  if constexpr (requires { std::forward<T>(value).operator co_await(); })
    return std::forward<T>(value).operator co_await();
  else if constexpr (requires { operator co_await(std::forward<T>(value)); })
    return operator co_await(std::forward<T>(value));
  else
    return std::forward<T>(value);
}
}
template <class T>
using awaitable_result_t = decltype(detail::get_awaiter(std::declval<T>()).await_resume());
/** @brief IO awaitable 必须通过 expected 明确返回结果或错误。 */
template <class A, class Result>
concept io_awaitable_of = requires(A&& operation) {
  { detail::get_awaiter(std::forward<A>(operation)).await_ready() } -> std::convertible_to<bool>;
  {
    detail::get_awaiter(std::forward<A>(operation)).await_resume()
  } -> std::same_as<expected<Result>>;
};
}
