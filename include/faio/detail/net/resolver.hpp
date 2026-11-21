/**
 * @file faio/detail/net/resolver.hpp
 * @brief 地址解析。
 */
#ifndef FAIO_DETAIL_NET_RESOLVER_HPP
#define FAIO_DETAIL_NET_RESOLVER_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/blocking/call.hpp"
#include "faio/detail/net/base/address_value.hpp"
#include <charconv>
#include <memory>
#include <string>
#include <vector>

namespace faio::net::detail {
/**
 *  @brief 拥有 DNS 参数的地址规格；构造时复制字符串，允许临时 string_view
 * 输入。
 */
struct HostPort {
  std::string host;    /**< 主机名或纯数字 IPv4/IPv6，可包含 IPv6 scope。 */
  std::string service; /**< 十进制端口或系统服务名。 */

  /** @brief 复制主机名与服务参数，供异步地址解析使用。 */
  HostPort(std::string_view hostname, std::uint16_t port)
      : host{hostname}, service{std::to_string(port)} {}

  /** @brief 复制主机名与服务参数，供异步地址解析使用。 */
  HostPort(std::string_view hostname, std::string_view service_name)
      : host{hostname}, service{service_name} {}
};

/**
 *  @brief 异步解析全部 IPv4/IPv6 端点；host 和 port 均为数字时不进入线程池。
 * @param context 创建时的 IO domain，提供独立 resolver 服务配额。
 * @param host 拥有主机名，NUL 字节被拒绝，避免 C 字符串截断导致查询错误主机。
 * @param service 拥有端口或服务名；端口零允许用于本地绑定。
 * @param socket_type SOCK_STREAM/SOCK_DGRAM，防止返回不符合传输协议的重复端点。
 * @details getaddrinfo 只在 resolver executor 上执行，不阻塞协程 worker。
 * 结果保留 resolver 错误域；EAI_SYSTEM 则保留实际的 errno。不提供 DNS TTL 缓存。
 */
inline auto lookup_host(io::io_context context,
                        std::string host,
                        std::string service,
                        int socket_type = SOCK_STREAM) -> task<expected<std::vector<SocketAddr>>> {
  if (host.find('\0') != std::string::npos || service.find('\0') != std::string::npos)
    co_return std::unexpected{make_error(EINVAL)};
  unsigned port{};
  const auto [end, error] = std::from_chars(service.data(), service.data() + service.size(), port);
  if (!service.empty() && error == std::errc{} && end == service.data() + service.size()
      && port <= 65535) {
    if (auto numeric = SocketAddr::numeric_parse(host, static_cast<std::uint16_t>(port)))
      co_return std::vector<SocketAddr>{*numeric};
  }
  auto resolver = context.resolver();
  co_return co_await blocking::call_on(
      context,
      [host = std::move(host),
       service = std::move(service),
       socket_type]() -> expected<std::vector<SocketAddr>> {
        if (auto initialized = initialize_resolver(); !initialized)
          return std::unexpected{initialized.error()};
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = socket_type;
        addrinfo* result{};
        const int failure = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
        if (failure != 0) {
          return std::unexpected{resolver_error(failure)};
        }
        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> owner{result, &::freeaddrinfo};
        std::vector<SocketAddr> addresses;
        for (const auto* entry = result; entry; entry = entry->ai_next) {
          if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6)
            continue;
          SocketAddr address{entry->ai_addr, entry->ai_addrlen};
          if (std::find(addresses.begin(), addresses.end(), address) == addresses.end())
            addresses.push_back(address);
        }
        if (addresses.empty())
          return std::unexpected{make_error(EADDRNOTAVAIL)};
        return addresses;
      },
      resolver);
}

/** @brief 解析主机名和端口，返回去重的 IPv4/IPv6 端点。 */
inline auto lookup_host(io::io_context context, std::string host, std::uint16_t port)
    -> task<expected<std::vector<SocketAddr>>> {
  co_return co_await lookup_host(std::move(context), std::move(host), std::to_string(port));
}

/** @brief 解析主机名和端口，返回去重的 IPv4/IPv6 端点。 */
inline auto lookup_host(std::string host, std::uint16_t port)
    -> task<expected<std::vector<SocketAddr>>> {
  co_return co_await lookup_host(io::io_context::current(), std::move(host), std::to_string(port));
}
}  // namespace faio::net::detail

namespace faio::net {
using HostPort = detail::HostPort;
using detail::lookup_host;
}  // namespace faio::net
#endif
