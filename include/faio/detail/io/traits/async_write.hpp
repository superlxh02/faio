/**
 * @file faio/detail/io/traits/async_write.hpp
 * @brief 异步写入概念。
 */
#pragma once
#include "faio/detail/io/traits/awaitable.hpp"
#include "faio/detail/io/base/buffer.hpp"

namespace faio::io {
template <class T>
concept async_writer = requires(T& writer, io_buffer buffer) {
  { writer.write(std::move(buffer)) } -> io_awaitable_of<io_transfer>;
};
template <class T>
concept borrowed_async_writer = requires(T& writer, std::span<const char> buffer) {
  { writer.write(buffer) } -> io_awaitable_of<std::size_t>;
};
}  // namespace faio::io
