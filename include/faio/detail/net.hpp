#ifndef FAIO_DETAIL_NET_HPP
#define FAIO_DETAIL_NET_HPP
#include "faio/detail/net/common/address.hpp"
#include "faio/detail/net/tcp/tcp_listener.hpp"
#include "faio/detail/net/tcp/tcp_stream.hpp"
#include "faio/detail/net/udp/datagram.hpp"

namespace faio::net {
using address = detail::SocketAddr;
using v4addr = detail::Ipv4Addr;
using v6addr = detail::Ipv6Addr;
using TcpListener = detail::TcpListener;
using TcpStream = detail ::TcpStream;
using UdpDatagram = detail::UdpDatagram;
} // namespace faio::net

#endif // FAIO_DETAIL_NET_HPP