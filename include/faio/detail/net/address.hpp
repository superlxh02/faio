/**
 * @file faio/detail/net/address.hpp
 * @brief 网络地址、socket 与传输协议：socket 地址接口。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
#include "faio/detail/net/base/address_value.hpp"

namespace faio::net {
using SocketAddr = detail::SocketAddr;
using Ipv4Addr = detail::Ipv4Addr;
using Ipv6Addr = detail::Ipv6Addr;
using address = detail::SocketAddr;
using v4addr = detail::Ipv4Addr;
using v6addr = detail::Ipv6Addr;
}
