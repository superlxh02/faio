/**
 * @file faio/detail/net/unix_domain.hpp
 * @brief Unix 域入口。
 */
#pragma once
#if !defined(_WIN32)
#include "faio/detail/net/unix_domain/address.hpp"
#include "faio/detail/net/unix_domain/socket.hpp"

namespace faio::net::unix {
using address = detail::unix_addr;
using unix_addr = detail::unix_addr;
using unix_socket = detail::unix_socket;
using unix_listener = detail::unix_listener;
using unix_stream = detail::unix_stream;
using unix_datagram = detail::unix_datagram;
using peer_credentials = detail::peer_credentials;
}  // namespace faio::net::unix
#endif
