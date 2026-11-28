/**
 * @file faio/detail/net/tcp.hpp
 * @brief TCP 入口。
 */
#pragma once
#include "faio/detail/net/address.hpp"
#include "faio/detail/net/resolver.hpp"
#include "faio/detail/net/tcp/socket.hpp"
#include "faio/detail/net/tcp/listener.hpp"
#include "faio/detail/net/tcp/stream.hpp"

namespace faio::net {
using owned_native_socket = detail::owned_native_socket;
using tcp_socket = detail::tcp_socket;
using tcp_listener = detail::tcp_listener;
using tcp_stream = detail::tcp_stream;
}  // namespace faio::net
