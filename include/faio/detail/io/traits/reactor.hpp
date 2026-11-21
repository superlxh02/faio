/**
 * @file faio/detail/io/traits/reactor.hpp
 * @brief 就绪反应器。
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

}  // namespace faio::io::detail
