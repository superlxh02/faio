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
using TcpSocket = detail::TcpSocket;
using TcpListener = detail::TcpListener;
using TcpStream = detail::TcpStream;
}  // namespace faio::net
