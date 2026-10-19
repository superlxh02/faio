/**
 * @file faio/detail/io/traits/reactor.hpp
 * @brief 跨平台异步 IO：就绪反应器。
 * @details 以编译期约束检查接口形状及返回类型；概念检查本身不执行 IO，也不承担资源生命周期。
 */
#pragma once
#include "faio/detail/io/protocol/reactor.hpp"
#include <concepts>
#include <optional>
#include <span>

namespace faio::io::detail {
template <class R>
concept reactor_implementation = requires(R& object,
                                          int fd,
                                          std::uint64_t key,
                                          std::span<readiness_event> out,
                                          std::optional<int> timeout) {
  { R::backend_name } -> std::convertible_to<const char*>;
  { object.register_resource(fd, key) } noexcept -> std::same_as<int>;
  { object.unregister_resource(fd) } noexcept;
  { object.poll(out, timeout) } noexcept -> std::same_as<int>;
  { object.wake() } noexcept;
};

}
