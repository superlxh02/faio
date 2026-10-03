#pragma once
#include "faio/detail/io/traits/async_read.hpp"
namespace faio::io {
template <class T>
concept async_writer = requires(T &writer, io_buffer buffer) {
  { writer.write(std::move(buffer)) } -> io_awaitable_of<io_transfer>;
};
template <class T>
concept borrowed_async_writer =
    requires(T &writer, std::span<const char> buffer) {
      { writer.write(buffer) } -> io_awaitable_of<std::size_t>;
    };
} // namespace faio::io
