/**
 * @file faio/detail/net/address.hpp
 * @brief socket 地址接口。
 */
#pragma once
#include "faio/detail/net/base/address_value.hpp"

namespace faio::net {
using socket_addr = detail::socket_addr;
using ipv4_addr = detail::ipv4_addr;
using ipv6_addr = detail::ipv6_addr;
using address = detail::socket_addr;
using v4addr = detail::ipv4_addr;
using v6addr = detail::ipv6_addr;
}  // namespace faio::net
