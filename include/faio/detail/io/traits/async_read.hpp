/**
 * @file faio/detail/io/traits/async_read.hpp
 * @brief 异步读取概念。
 */
#pragma once
#include "faio/detail/io/traits/awaitable.hpp"
#include "faio/detail/io/base/buffer.hpp"

namespace faio::io {
template <class T>
concept async_reader = requires(T& reader, io_buffer buffer) {
  { reader.read(std::move(buffer)) } -> io_awaitable_of<io_transfer>;
};
/**  @brief 零拷贝借用读取；概念本身不保证实例可并发操作。 */
template <class T>
concept borrowed_async_reader = requires(T& reader, std::span<char> buffer) {
  { reader.read(buffer) } -> io_awaitable_of<std::size_t>;
};

}  // namespace faio::io
