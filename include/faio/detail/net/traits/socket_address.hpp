/**
 * @file faio/detail/net/traits/socket_address.hpp
 * @brief 地址类型概念。
 */
#pragma once
#include "faio/detail/net/platform/select.hpp"
#include <concepts>

namespace faio::net::detail {
template <class Address>
concept socket_address = requires(Address address) {
  { address.sockaddr() } noexcept -> std::same_as<sockaddr*>;
  { address.length() } noexcept -> std::same_as<socklen_t>;
};
}  // namespace faio::net::detail
