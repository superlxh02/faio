/**
 * @file faio/detail/net/tcp.hpp
 * @brief 网络地址、socket 与传输协议：TCP 入口。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
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
}
