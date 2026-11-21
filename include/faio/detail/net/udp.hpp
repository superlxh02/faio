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
using UdpSocket = detail::UdpSocket;
using UdpDatagram = detail::UdpDatagram;
using DatagramMessage = detail::DatagramMessage<detail::SocketAddr>;
using OwnedDatagram = detail::OwnedDatagram<detail::SocketAddr>;
using DatagramSend = detail::DatagramSend<detail::SocketAddr>;
}  // namespace faio::net
