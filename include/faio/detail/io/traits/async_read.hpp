/**
 * @file faio/detail/io/traits/async_read.hpp
 * @brief 跨平台异步 IO：异步读取概念。
 * @details 以编译期约束检查接口形状及返回类型；概念检查本身不执行 IO，也不承担资源生命周期。
 */
#pragma once
#include "faio/detail/io/traits/awaitable.hpp"

namespace faio::io {
template <class T>
concept async_reader = requires(T& reader, io_buffer buffer) {
  { reader.read(std::move(buffer)) } -> io_awaitable_of<io_transfer>;
};
/** @brief 零拷贝借用读取；概念本身不保证实例可并发操作。 */
template <class T>
concept borrowed_async_reader = requires(T& reader, std::span<char> buffer) {
  { reader.read(buffer) } -> io_awaitable_of<std::size_t>;
};

}
