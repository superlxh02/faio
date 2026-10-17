#include <faio/net.hpp>
#include <type_traits>
#include <variant>

static_assert(!std::is_copy_constructible_v<faio::net::TcpStream>);
static_assert(std::is_move_constructible_v<faio::net::TcpStream>);

bool module_net_public_header_smoke() {
  const faio::net::Ipv4Addr loopback{127, 0, 0, 1};
  faio::net::SocketAddr endpoint{loopback, 4321};
  endpoint.set_port(8765);
  return endpoint.is_ipv4() && endpoint.port() == 8765
         && std::get<faio::net::Ipv4Addr>(endpoint.ip()) == loopback;
}
