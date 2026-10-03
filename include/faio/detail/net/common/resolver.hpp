#ifndef FAIO_DETAIL_NET_COMMON_RESOLVER_HPP
#define FAIO_DETAIL_NET_COMMON_RESOLVER_HPP
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/execution/execute.hpp"
#include "faio/detail/net/common/address.hpp"
#include <charconv>
#include <memory>
#include <string>
#include <vector>
namespace faio::net::detail {
/** @brief 拥有 DNS 参数的地址规格；构造时复制字符串，允许临时 string_view
 * 输入。 */
struct HostPort {
  std::string host;    ///< 主机名或纯数字 IPv4/IPv6，可包含 IPv6 scope。
  std::string service; ///< 十进制端口或系统服务名。
  HostPort(std::string_view hostname, std::uint16_t port)
      : host{hostname}, service{std::to_string(port)} {}
  HostPort(std::string_view hostname, std::string_view service_name)
      : host{hostname}, service{service_name} {}
};
/** @brief 异步解析全部 IPv4/IPv6 端点；数字 host + 数字 port
 * 的路径不进入线程池。
 * @param context 创建时的 IO domain，提供独立 resolver 服务配额。
 * @param host 拥有主机名，NUL 字节被拒绝，避免 C 字符串截断导致查询错误主机。
 * @param service 拥有端口或服务名；端口零允许用于本地绑定。
 * @param socket_type SOCK_STREAM/SOCK_DGRAM，防止返回不符合传输协议的重复端点。
 * @details getaddrinfo 只在 resolver executor 上执行，不阻塞协程 worker。
 * 返回保留 resolver 错误域；EAI_SYSTEM 则保留真正 errno。没有伪造 DNS TTL
 * 缓存。
 */
inline auto lookup_host(io::io_context context, std::string host,
                        std::string service, int socket_type = SOCK_STREAM)
    -> task<expected<std::vector<SocketAddr>>> {
  if (host.find('\0') != std::string::npos ||
      service.find('\0') != std::string::npos)
    co_return std::unexpected{make_error(EINVAL)};
  unsigned port{};
  const auto [end, error] =
      std::from_chars(service.data(), service.data() + service.size(), port);
  if (!service.empty() && error == std::errc{} &&
      end == service.data() + service.size() && port <= 65535) {
    if (auto numeric =
            SocketAddr::numeric_parse(host, static_cast<std::uint16_t>(port)))
      co_return std::vector<SocketAddr>{*numeric};
  }
  // 参数按值移入 job；取消仍保持 job 所有权，已开始的解析安全排空后再恢复。
  auto resolver = context.resolver();
  co_return co_await execution::execute_blocking(
      context,
      [host = std::move(host), service = std::move(service),
       socket_type]() -> expected<std::vector<SocketAddr>> {
        addrinfo
            hints{}; // 仅要求 IP，不把当前机器没有公网接口解释成地址不可用。
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = socket_type;
        addrinfo *result{};
        const int failure =
            ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
        if (failure != 0) {
          if (failure == EAI_SYSTEM)
            return std::unexpected{make_error(errno)};
          return std::unexpected{Error{failure, 0, error_domain::resolver}};
        }
        // 即使 vector 分配失败，RAII 仍归还整个链表。
        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> owner{
            result, &::freeaddrinfo};
        std::vector<SocketAddr> addresses;
        for (const auto *entry = result; entry; entry = entry->ai_next) {
          if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6)
            continue;
          SocketAddr address{entry->ai_addr, entry->ai_addrlen};
          if (std::find(addresses.begin(), addresses.end(), address) ==
              addresses.end())
            addresses.push_back(address);
        }
        if (addresses.empty())
          return std::unexpected{make_error(EADDRNOTAVAIL)};
        return addresses;
      },
      resolver);
}
inline auto lookup_host(io::io_context context, std::string host,
                        std::uint16_t port)
    -> task<expected<std::vector<SocketAddr>>> {
  co_return co_await lookup_host(std::move(context), std::move(host),
                                 std::to_string(port));
}
inline auto lookup_host(std::string host, std::uint16_t port)
    -> task<expected<std::vector<SocketAddr>>> {
  co_return co_await lookup_host(io::io_context::current(), std::move(host),
                                 std::to_string(port));
}
} // namespace faio::net::detail
#endif
