/**
 * @file faio/detail/io/traits/async_buf_read.hpp
 * @brief 跨平台异步 IO：缓冲读取概念。
 * @details 以编译期约束检查接口形状及返回类型；概念检查本身不执行 IO，也不承担资源生命周期。
 */
#pragma once
#include "faio/detail/io/traits/awaitable.hpp"

namespace faio::io {
/** @brief 返回已初始化借用视图的缓冲读取概念；视图生命周期由具体类型规定。 */
template <class T>
concept async_buf_reader = requires(T& reader, std::size_t count) {
  { reader.fill_buf() } -> io_awaitable_of<std::span<const char>>;
  { reader.consume(count) } -> std::same_as<void>;
};
}
