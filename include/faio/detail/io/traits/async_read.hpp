#pragma once
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/buffer.hpp"
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
}  // namespace detail
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
template <class T>
concept async_reader = requires(T& reader, io_buffer buffer) {
  { reader.read(std::move(buffer)) } -> io_awaitable_of<io_transfer>;
};
/** @brief 零拷贝借用读取；概念本身不保证实例可并发操作。 */
template <class T>
concept borrowed_async_reader = requires(T& reader, std::span<char> buffer) {
  { reader.read(buffer) } -> io_awaitable_of<std::size_t>;
};
template <class T>
concept async_seeker = requires(T& reader, std::uint64_t offset) {
  { reader.seek(offset) } -> io_awaitable_of<std::uint64_t>;
};
}  // namespace faio::io
