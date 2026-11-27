module;

#include <faio/tls/handshake.hpp>
#include <faio/tls/stream.hpp>

export module faio.tls;
export import faio.net;
export import faio.runtime;

export namespace faio::tls {
using ::faio::tls::ClientAuth;
using ::faio::tls::ClientConfig;
using ::faio::tls::ClientConfigBuilder;
using ::faio::tls::ConnectionInfo;
using ::faio::tls::errc;
using ::faio::tls::Identity;
using ::faio::tls::ServerConfig;
using ::faio::tls::ServerConfigBuilder;
using ::faio::tls::ServerName;
using ::faio::tls::TlsAcceptor;
using ::faio::tls::TlsConnector;
using ::faio::tls::TlsReadHalf;
using ::faio::tls::TlsStream;
using ::faio::tls::TlsVersion;
using ::faio::tls::TlsWriteHalf;
using ::faio::tls::TrustStore;
}  // namespace faio::tls
