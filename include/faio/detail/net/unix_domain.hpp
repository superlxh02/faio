/**
 * @file faio/detail/net/unix_domain.hpp
 * @brief 网络地址、socket 与传输协议：Unix 域入口。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
/// @brief Unix domain socket 是传输协议；操作系统差异由 platform 处理。
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
}
#endif
