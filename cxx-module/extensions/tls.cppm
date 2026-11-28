module;

#include <faio/tls/handshake.hpp>
#include <faio/tls/stream.hpp>

export module faio.tls;
export import faio.net;
export import faio.runtime;

export namespace faio::tls {
using ::faio::tls::client_auth;
using ::faio::tls::client_config;
using ::faio::tls::client_config_builder;
using ::faio::tls::connection_info;
using ::faio::tls::errc;
using ::faio::tls::identity;
using ::faio::tls::server_config;
using ::faio::tls::server_config_builder;
using ::faio::tls::server_name;
using ::faio::tls::tls_acceptor;
using ::faio::tls::tls_connector;
using ::faio::tls::tls_read_half;
using ::faio::tls::tls_stream;
using ::faio::tls::tls_version;
using ::faio::tls::tls_write_half;
using ::faio::tls::trust_store;
}  // namespace faio::tls
