/**
 * @file faio/detail/io/traits/async_seek.hpp
 * @brief 异步定位概念。
 */
#pragma once
#include "faio/detail/io/traits/awaitable.hpp"
#include <cstdint>

namespace faio::io {
template <class T>
concept async_seeker = requires(T& reader, std::uint64_t offset) {
  { reader.seek(offset) } -> io_awaitable_of<std::uint64_t>;
};

}  // namespace faio::io
