/**
 * @file faio/detail/io/traits/native_fd_owner.hpp
 * @brief 跨平台异步 IO：原生句柄拥有者概念。
 * @details 以编译期约束检查接口形状及返回类型；概念检查本身不执行 IO，也不承担资源生命周期。
 */
#pragma once
#include <concepts>
#include <type_traits>

#ifdef unix
#undef unix
#endif
namespace faio::io::unix {
/** @brief 可交接描述符的原生拥有者；重建不抛出，避免导出后丢失关闭责任。 */
template <class Owner>
concept native_fd_owner = std::is_nothrow_constructible_v<Owner, int> && requires(Owner& owner) {
  { owner.get() } -> std::same_as<int>;
  { owner.release() } noexcept -> std::same_as<int>;
};
}
