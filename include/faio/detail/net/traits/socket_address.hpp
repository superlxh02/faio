/**
 * @file faio/detail/net/traits/socket_address.hpp
 * @brief 网络地址、socket 与传输协议：地址类型概念。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
#include "faio/detail/net/platform/select.hpp"
#include <concepts>

namespace faio::net::detail {
template <class Address>
concept socket_address = requires(Address address) {
  { address.sockaddr() } noexcept -> std::same_as<sockaddr*>;
  { address.length() } noexcept -> std::same_as<socklen_t>;
};
}
