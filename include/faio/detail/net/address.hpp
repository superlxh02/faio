/**
 * @file faio/detail/net/address.hpp
 * @brief socket 地址接口。
 */
#pragma once
#include "faio/detail/net/base/address_value.hpp"

namespace faio::net {
using SocketAddr = detail::SocketAddr;
using Ipv4Addr = detail::Ipv4Addr;
using Ipv6Addr = detail::Ipv6Addr;
using address = detail::SocketAddr;
using v4addr = detail::Ipv4Addr;
using v6addr = detail::Ipv6Addr;
}  // namespace faio::net
