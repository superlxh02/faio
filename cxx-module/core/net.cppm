module;

#define FAIO_DETAIL_SEPARATE_DUPLEX 1
#include <faio/net/address.hpp>
#include <faio/net/local.hpp>
#include <faio/net/tcp.hpp>
#include <faio/net/udp.hpp>

export module faio.net;
export import faio.io;

export namespace faio::net {
using ::faio::net::DatagramMessage;
using ::faio::net::DatagramSend;
using ::faio::net::HostPort;
using ::faio::net::Ipv4Addr;
using ::faio::net::Ipv6Addr;
using ::faio::net::OwnedDatagram;
using ::faio::net::SocketAddr;
using ::faio::net::TcpListener;
using ::faio::net::TcpSocket;
using ::faio::net::TcpStream;
using ::faio::net::UdpDatagram;
using ::faio::net::UdpSocket;
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
using ::faio::net::unix::PeerCredentials;
using ::faio::net::unix::UnixAddr;
using ::faio::net::unix::UnixDatagram;
using ::faio::net::unix::UnixListener;
using ::faio::net::unix::UnixSocket;
using ::faio::net::unix::UnixStream;
using ::faio::net::unix::address;
}  // namespace faio::net::unix
#endif

#if !defined(_WIN32)
export namespace faio::net::unix::pipe {
using ::faio::net::unix::pipe::Receiver;
using ::faio::net::unix::pipe::Sender;
using ::faio::net::unix::pipe::owned_native_fd;
using ::faio::net::unix::pipe::pair;
}  // namespace faio::net::unix::pipe
#endif
