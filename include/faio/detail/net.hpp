#ifndef FAIO_DETAIL_NET_HPP
#define FAIO_DETAIL_NET_HPP
#include "faio/detail/net/common/accept_options.hpp"
#include "faio/detail/net/common/address.hpp"
#include "faio/detail/net/tcp/socket.hpp"
#include "faio/detail/net/tcp/tcp_listener.hpp"
#include "faio/detail/net/tcp/tcp_stream.hpp"
#include "faio/detail/net/udp/datagram.hpp"
#if !defined(_WIN32)
#include "faio/detail/net/unix/pipe.hpp"
#include "faio/detail/net/unix/socket.hpp"
#endif

namespace faio::net {
using SocketAddr = detail::SocketAddr;
using Ipv4Addr = detail::Ipv4Addr;
using Ipv6Addr = detail::Ipv6Addr;
using HostPort = detail::HostPort;
using detail::lookup_host;
using owned_native_socket = detail::owned_native_socket;
using TcpSocket = detail::TcpSocket;
using UdpSocket = detail::UdpSocket;
using DatagramMessage = detail::DatagramMessage<detail::SocketAddr>;
using OwnedDatagram = detail::OwnedDatagram<detail::SocketAddr>;
using DatagramSend = detail::DatagramSend<detail::SocketAddr>;
using address = detail::SocketAddr;
using v4addr = detail::Ipv4Addr;
using v6addr = detail::Ipv6Addr;
using TcpListener = detail::TcpListener;
using TcpStream = detail ::TcpStream;
using UdpDatagram = detail::UdpDatagram;
} // namespace faio::net

#endif // FAIO_DETAIL_NET_HPP
