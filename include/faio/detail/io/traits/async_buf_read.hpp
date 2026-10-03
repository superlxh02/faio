#pragma once
#include "faio/detail/io/traits/async_read.hpp"

namespace faio::io {
/** @brief 返回已初始化借用视图的缓冲读取概念；视图生命周期由具体类型规定。 */
template <class T>
concept async_buf_reader = requires(T& reader, std::size_t count) {
  { reader.fill_buf() } -> io_awaitable_of<std::span<const char>>;
  { reader.consume(count) } -> std::same_as<void>;
};
}  // namespace faio::io
