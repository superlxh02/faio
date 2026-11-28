/**
 * @file faio/detail/net/udp.hpp
 * @brief UDP 入口。
 */
#pragma once
#include "faio/detail/net/address.hpp"
#include "faio/detail/net/resolver.hpp"
#include "faio/detail/net/udp/socket.hpp"

namespace faio::net {
using owned_native_socket = detail::owned_native_socket;
using udp_socket = detail::udp_socket;
using udp_datagram = detail::udp_datagram;
using datagram_message = detail::datagram_message<detail::socket_addr>;
using owned_datagram = detail::owned_datagram<detail::socket_addr>;
using datagram_send = detail::datagram_send<detail::socket_addr>;
}  // namespace faio::net
