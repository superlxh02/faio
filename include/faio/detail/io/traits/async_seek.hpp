/**
 * @file faio/detail/io/traits/async_seek.hpp
 * @brief 跨平台异步 IO：异步定位概念。
 * @details 以编译期约束检查接口形状及返回类型；概念检查本身不执行 IO，也不承担资源生命周期。
 */
#pragma once
#include "faio/detail/io/traits/awaitable.hpp"

namespace faio::io {
template <class T>
concept async_seeker = requires(T& reader, std::uint64_t offset) {
  { reader.seek(offset) } -> io_awaitable_of<std::uint64_t>;
};

}
