/**
 * @file faio/detail/io/traits/async_write.hpp
 * @brief 跨平台异步 IO：异步写入概念。
 * @details 以编译期约束检查接口形状及返回类型；概念检查本身不执行 IO，也不承担资源生命周期。
 */
#pragma once
#include "faio/detail/io/traits/awaitable.hpp"

namespace faio::io {
template <class T>
concept async_writer = requires(T& writer, io_buffer buffer) {
  { writer.write(std::move(buffer)) } -> io_awaitable_of<io_transfer>;
};
template <class T>
concept borrowed_async_writer = requires(T& writer, std::span<const char> buffer) {
  { writer.write(buffer) } -> io_awaitable_of<std::size_t>;
};
}
