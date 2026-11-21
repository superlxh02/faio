/**
 * @file faio/detail/net/unix_domain.hpp
 * @brief Unix 域入口。
 */
#pragma once
#if !defined(_WIN32)
#include "faio/detail/net/unix_domain/address.hpp"
#include "faio/detail/net/unix_domain/socket.hpp"

namespace faio::net::unix {
using address = detail::UnixAddr;
using UnixAddr = detail::UnixAddr;
using UnixSocket = detail::UnixSocket;
using UnixListener = detail::UnixListener;
using UnixStream = detail::UnixStream;
using UnixDatagram = detail::UnixDatagram;
using PeerCredentials = detail::PeerCredentials;
}  // namespace faio::net::unix
#endif
