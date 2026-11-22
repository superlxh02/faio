#include <chrono>
#include <coroutine>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#if defined(FAIO_USE_HEADERS) && FAIO_USE_HEADERS
#include <faio/coroutine/composition.hpp>
#include <faio/net/tcp.hpp>
#include <faio/runtime/default.hpp>
#include <faio/tls/handshake.hpp>
#else
import faio;
import faio.tls;
#endif

namespace {
using namespace std::chrono_literals;

faio::task<bool> accept_client(faio::net::TcpListener& listener, faio::tls::ServerConfig config) {
  auto accepted = co_await listener.accept().set_timeout(5s);
  if (!accepted)
    co_return false;
  const faio::tls::TlsAcceptor acceptor{std::move(config)};
  const auto stream = co_await acceptor.accept(std::move(std::get<0>(*accepted)));
  co_return !stream;
}

faio::task<bool> check_client(faio::net::SocketAddr endpoint, faio::tls::ClientConfig config) {
  auto socket = co_await faio::net::TcpStream::connect(endpoint);
  if (!socket)
    co_return false;
  const faio::tls::TlsConnector connector{std::move(config)};
  auto name = faio::tls::ServerName::parse("wrong-host.invalid").value();
  const auto stream = co_await connector.connect(std::move(name), std::move(*socket));
  if (stream)
    co_return false;
  const auto& error = stream.error();
  co_return error.domain() == faio::error_domain::tls
      && error.value() == static_cast<int>(faio::tls::errc::hostname_mismatch) && error.context()
      && error.context()->verify_code.has_value();
}

faio::task<bool> reject_wrong_hostname(faio::tls::ClientConfig client,
                                       faio::tls::ServerConfig server) {
  const faio::net::SocketAddr loopback{faio::net::Ipv4Addr{127, 0, 0, 1}, 0};
  auto listener = faio::net::TcpListener::bind(loopback);
  if (!listener)
    co_return false;
  const auto endpoint = listener->local_addr().value();
  const auto [client_rejected, server_closed] =
      co_await faio::join(check_client(endpoint, std::move(client)),
                          accept_client(*listener, std::move(server)));
  co_return (co_await listener->close()) && client_rejected && server_closed;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4)
    return 2;
  const auto name = faio::tls::ServerName::parse("LOCALHOST.");
  if (!name || name->value() != "localhost" || name->is_ip()
      || faio::tls::ServerName::parse("*.localhost"))
    return 1;
  auto identity = faio::tls::Identity::from_pem_files(argv[1], argv[2]);
  auto roots = faio::tls::TrustStore::from_pem_file(argv[3]);
  if (!identity || !roots || faio::tls::TrustStore::from_pem("invalid CA"))
    return 1;
  auto config = faio::tls::ClientConfig::builder(*roots).with_alpn({"h2"}).build();
  auto server = faio::tls::ServerConfig::builder(std::move(*identity)).build();
  if (!config || !server || config->alpn_protocols().size() != 1
      || config->alpn_protocols().front() != "h2")
    return 1;
  return faio::block_on(reject_wrong_hostname(std::move(*config), std::move(*server))) ? 0 : 1;
}
