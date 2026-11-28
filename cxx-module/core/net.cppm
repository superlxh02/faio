module;

#include <faio/net/address.hpp>
#include <faio/net/local.hpp>
#include <faio/net/tcp.hpp>
#include <faio/net/udp.hpp>

export module faio.net;
export import faio.io;

export namespace faio::net {
using ::faio::net::datagram_message;
using ::faio::net::datagram_send;
using ::faio::net::host_port;
using ::faio::net::ipv4_addr;
using ::faio::net::ipv6_addr;
using ::faio::net::owned_datagram;
using ::faio::net::socket_addr;
using ::faio::net::tcp_listener;
using ::faio::net::tcp_socket;
using ::faio::net::tcp_stream;
using ::faio::net::udp_datagram;
using ::faio::net::udp_socket;
using ::faio::net::accept_options;
using ::faio::net::accept_placement;
using ::faio::net::address;
using ::faio::net::lookup_host;
using ::faio::net::owned_native_socket;
using ::faio::net::v4addr;
using ::faio::net::v6addr;
}  // namespace faio::net

#if !defined(_WIN32)
export namespace faio::net::unix {
using ::faio::net::unix::peer_credentials;
using ::faio::net::unix::unix_addr;
using ::faio::net::unix::unix_datagram;
using ::faio::net::unix::unix_listener;
using ::faio::net::unix::unix_socket;
using ::faio::net::unix::unix_stream;
using ::faio::net::unix::address;
}  // namespace faio::net::unix
#endif

#if !defined(_WIN32)
export namespace faio::net::unix::pipe {
using ::faio::net::unix::pipe::receiver;
using ::faio::net::unix::pipe::sender;
using ::faio::net::unix::pipe::owned_native_fd;
using ::faio::net::unix::pipe::pair;
}  // namespace faio::net::unix::pipe
#endif
