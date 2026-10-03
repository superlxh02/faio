#ifndef FAIO_DETAIL_NET_COMMON_ADDRESS_HPP
#define FAIO_DETAIL_NET_COMMON_ADDRESS_HPP

#include "faio/detail/common/error.hpp"
#include <arpa/inet.h>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <net/if.h>
#include <netdb.h>
#include <string>
#include <variant>

namespace faio::net::detail {
/** @brief IPv4 值类型；内部保存网络字节序，避免依赖 CPU 大小端。 */
class Ipv4Addr {
public:
  Ipv4Addr() noexcept = default;
  Ipv4Addr(std::uint8_t a, std::uint8_t b, std::uint8_t c,
           std::uint8_t d) noexcept {
    const std::array bytes{a, b, c, d};
    std::memcpy(&ip_, bytes.data(), bytes.size());
  }
  explicit Ipv4Addr(std::uint32_t network_order) noexcept
      : ip_{network_order} {}
  [[nodiscard]] auto addr() const noexcept -> std::uint32_t { return ip_; }
  [[nodiscard]] auto octets() const noexcept -> std::array<std::uint8_t, 4> {
    std::array<std::uint8_t, 4> result{};
    std::memcpy(result.data(), &ip_, result.size());
    return result;
  }
  [[nodiscard]] auto to_string() const -> std::string {
    std::array<char, INET_ADDRSTRLEN> buffer{};
    if (!::inet_ntop(AF_INET, &ip_, buffer.data(), buffer.size()))
      return {};
    return buffer.data();
  }
  /** @brief 严格解析数字地址；string_view 不要求以 NUL 结尾。 */
  [[nodiscard]] static auto parse(std::string_view text) -> expected<Ipv4Addr> {
    std::uint32_t address{};
    if (text.find('\0') != std::string_view::npos)
      return std::unexpected{make_error(EINVAL)};
    const std::string owned{text};
    if (::inet_pton(AF_INET, owned.c_str(), &address) != 1)
      return std::unexpected{make_error(EINVAL)};
    return Ipv4Addr{address};
  }
  [[nodiscard]] auto is_unspecified() const noexcept -> bool {
    return ip_ == 0;
  }
  [[nodiscard]] auto is_loopback() const noexcept -> bool {
    return octets()[0] == 127;
  }
  [[nodiscard]] auto is_multicast() const noexcept -> bool {
    return (octets()[0] & 0xf0) == 0xe0;
  }
  friend auto operator==(const Ipv4Addr &, const Ipv4Addr &) noexcept
      -> bool = default;

private:
  std::uint32_t ip_{};
};

/** @brief IPv6 值类型，使用标准 s6_addr 而非 Linux 内部 union 字段。 */
class Ipv6Addr {
public:
  Ipv6Addr() noexcept = default;
  Ipv6Addr(std::uint16_t a, std::uint16_t b, std::uint16_t c, std::uint16_t d,
           std::uint16_t e, std::uint16_t f, std::uint16_t g,
           std::uint16_t h) noexcept {
    const std::array<std::uint16_t, 8> words{htons(a), htons(b), htons(c),
                                             htons(d), htons(e), htons(f),
                                             htons(g), htons(h)};
    std::memcpy(ip_.s6_addr, words.data(), sizeof(ip_));
  }
  explicit Ipv6Addr(in6_addr ip) noexcept : ip_{ip} {}
  [[nodiscard]] auto addr() const noexcept -> const in6_addr & { return ip_; }
  [[nodiscard]] auto to_string() const -> std::string {
    std::array<char, INET6_ADDRSTRLEN> buffer{};
    if (!::inet_ntop(AF_INET6, &ip_, buffer.data(), buffer.size()))
      return {};
    return buffer.data();
  }
  [[nodiscard]] static auto parse(std::string_view text) -> expected<Ipv6Addr> {
    in6_addr address{};
    if (text.find('\0') != std::string_view::npos)
      return std::unexpected{make_error(EINVAL)};
    const std::string owned{text};
    if (::inet_pton(AF_INET6, owned.c_str(), &address) != 1)
      return std::unexpected{make_error(EINVAL)};
    return Ipv6Addr{address};
  }
  [[nodiscard]] auto is_unspecified() const noexcept -> bool {
    return IN6_IS_ADDR_UNSPECIFIED(&ip_);
  }
  [[nodiscard]] auto is_loopback() const noexcept -> bool {
    return IN6_IS_ADDR_LOOPBACK(&ip_);
  }
  [[nodiscard]] auto is_multicast() const noexcept -> bool {
    return IN6_IS_ADDR_MULTICAST(&ip_);
  }
  friend auto operator==(const Ipv6Addr &a, const Ipv6Addr &b) noexcept
      -> bool {
    return std::memcmp(&a.ip_, &b.ip_, sizeof(in6_addr)) == 0;
  }

private:
  in6_addr ip_{};
};

/** @brief 拥有型 IP 端点，完整保存 IPv6 scope id 与 flow info。 */
class SocketAddr {
public:
  SocketAddr() noexcept = default;
  SocketAddr(const struct sockaddr *address, std::size_t length) noexcept {
    // 外部 sockaddr 的长度先校验再复制，绝不越过固定容量。
    if (address &&
        length >= offsetof(struct sockaddr, sa_family) +
                      sizeof(decltype(::sockaddr{}.sa_family)) &&
        length <= sizeof(storage_) &&
        ((address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) ||
         (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6))))
      std::memcpy(&storage_, address, length);
  }
  SocketAddr(Ipv4Addr ip, std::uint16_t port) noexcept {
    auto *address = reinterpret_cast<sockaddr_in *>(&storage_);
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    address->sin_addr.s_addr = ip.addr();
#if defined(__APPLE__) || defined(__FreeBSD__)
    address->sin_len = sizeof(sockaddr_in);
#endif
  }
  SocketAddr(const Ipv6Addr &ip, std::uint16_t port,
             std::uint32_t scope = 0) noexcept {
    auto *address = reinterpret_cast<sockaddr_in6 *>(&storage_);
    address->sin6_family = AF_INET6;
    address->sin6_port = htons(port);
    address->sin6_addr = ip.addr();
    address->sin6_scope_id = scope;
#if defined(__APPLE__) || defined(__FreeBSD__)
    address->sin6_len = sizeof(sockaddr_in6);
#endif
  }
  [[nodiscard]] auto ip() const noexcept -> std::variant<Ipv4Addr, Ipv6Addr> {
    if (is_ipv4())
      return Ipv4Addr{v4().sin_addr.s_addr};
    return Ipv6Addr{v6().sin6_addr};
  }
  void set_ip(Ipv4Addr ip) noexcept { *this = SocketAddr{ip, port()}; }
  void set_ip(const Ipv6Addr &ip) noexcept {
    *this = SocketAddr{ip, port(), scope_id()};
  }
  [[nodiscard]] auto port() const noexcept -> std::uint16_t {
    return is_ipv4() ? ntohs(v4().sin_port) : ntohs(v6().sin6_port);
  }
  void set_port(std::uint16_t value) noexcept {
    if (is_ipv4())
      reinterpret_cast<sockaddr_in *>(&storage_)->sin_port = htons(value);
    else
      reinterpret_cast<sockaddr_in6 *>(&storage_)->sin6_port = htons(value);
  }
  [[nodiscard]] auto scope_id() const noexcept -> std::uint32_t {
    return is_ipv6() ? v6().sin6_scope_id : 0;
  }
  void set_scope_id(std::uint32_t value) noexcept {
    if (is_ipv6())
      reinterpret_cast<sockaddr_in6 *>(&storage_)->sin6_scope_id = value;
  }
  [[nodiscard]] auto to_string() const -> std::string {
    if (is_ipv4())
      return Ipv4Addr{v4().sin_addr.s_addr}.to_string() + ":" +
             std::to_string(port());
    if (is_ipv6()) {
      auto host = Ipv6Addr{v6().sin6_addr}.to_string();
      if (scope_id())
        host += "%" + std::to_string(scope_id());
      return "[" + host + "]:" + std::to_string(port());
    }
    return {};
  }
  [[nodiscard]] auto is_ipv4() const noexcept -> bool {
    return family() == AF_INET;
  }
  [[nodiscard]] auto is_ipv6() const noexcept -> bool {
    return family() == AF_INET6;
  }
  [[nodiscard]] auto family() const noexcept -> int {
    return storage_.ss_family;
  }
  [[nodiscard]] auto sockaddr() const noexcept -> const struct sockaddr * {
    return reinterpret_cast<const struct sockaddr *>(&storage_);
  }
  [[nodiscard]] auto sockaddr() noexcept -> struct sockaddr * {
    return reinterpret_cast<struct sockaddr *>(&storage_);
  }
  [[nodiscard]] auto length() const noexcept -> socklen_t {
    return is_ipv4()   ? sizeof(sockaddr_in)
           : is_ipv6() ? sizeof(sockaddr_in6)
                       : sizeof(sockaddr_storage);
  }
  [[nodiscard]] static constexpr auto capacity() noexcept -> socklen_t {
    return sizeof(sockaddr_storage);
  }
  /** @brief 数字地址解析；支持 IPv6 的 %数字 / %接口名作用域，不执行 DNS。 */
  [[nodiscard]] static auto numeric_parse(std::string_view host,
                                          std::uint16_t port)
      -> expected<SocketAddr> {
    // 在拆分 scope 后缀前拒绝 NUL，避免 if_nametoindex 将接口名静默截断。
    if (host.find('\0') != std::string_view::npos)
      return std::unexpected{make_error(EINVAL)};
    if (auto ip = Ipv4Addr::parse(host))
      return SocketAddr{*ip, port};
    std::uint32_t scope{};
    if (const auto pos = host.find('%'); pos != std::string_view::npos) {
      const auto suffix = host.substr(pos + 1);
      auto [end, error] =
          std::from_chars(suffix.data(), suffix.data() + suffix.size(), scope);
      if (error != std::errc{} || end != suffix.data() + suffix.size()) {
        const std::string interface_name{suffix};
        scope = ::if_nametoindex(interface_name.c_str());
        if (!scope)
          return std::unexpected{make_error(EINVAL)};
      }
      host = host.substr(0, pos);
    }
    if (auto ip = Ipv6Addr::parse(host))
      return SocketAddr{*ip, port, scope};
    return std::unexpected{make_error(EINVAL)};
  }
  /** @brief 同步纯数字解析；域名应使用 lookup_host，不在 worker 中执行阻塞
   * DNS。 */
  [[nodiscard]] static auto parse(std::string_view host, std::uint16_t port)
      -> expected<SocketAddr> {
    return numeric_parse(host, port);
  }
  /** @brief 解析包含端口的数字端点，IPv6 要求 [地址]:端口 格式。 */
  [[nodiscard]] static auto parse(std::string_view endpoint)
      -> expected<SocketAddr> {
    std::string_view host;
    std::string_view service;
    if (endpoint.starts_with('[')) {
      const auto close = endpoint.find(']');
      if (close == std::string_view::npos || close + 1 >= endpoint.size() ||
          endpoint[close + 1] != ':')
        return std::unexpected{make_error(EINVAL)};
      host = endpoint.substr(1, close - 1);
      service = endpoint.substr(close + 2);
    } else {
      const auto colon = endpoint.find(':');
      if (colon == std::string_view::npos ||
          endpoint.find(':', colon + 1) != std::string_view::npos)
        return std::unexpected{make_error(EINVAL)};
      host = endpoint.substr(0, colon);
      service = endpoint.substr(colon + 1);
    }
    unsigned port{};
    const auto [end, error] =
        std::from_chars(service.data(), service.data() + service.size(), port);
    if (service.empty() || error != std::errc{} ||
        end != service.data() + service.size() || port > 65535)
      return std::unexpected{make_error(EINVAL)};
    return numeric_parse(host, static_cast<std::uint16_t>(port));
  }
  friend auto operator==(const SocketAddr &a, const SocketAddr &b) noexcept
      -> bool {
    if (a.family() != b.family() || a.port() != b.port())
      return false;
    if (a.is_ipv4())
      return a.v4().sin_addr.s_addr == b.v4().sin_addr.s_addr;
    if (a.is_ipv6())
      return a.scope_id() == b.scope_id() &&
             std::memcmp(&a.v6().sin6_addr, &b.v6().sin6_addr,
                         sizeof(in6_addr)) == 0;
    return true;
  }

private:
  [[nodiscard]] auto v4() const noexcept -> const sockaddr_in & {
    return *reinterpret_cast<const sockaddr_in *>(&storage_);
  }
  [[nodiscard]] auto v6() const noexcept -> const sockaddr_in6 & {
    return *reinterpret_cast<const sockaddr_in6 *>(&storage_);
  }
  sockaddr_storage storage_{};
};
} // namespace faio::net::detail

namespace std {
template <>
struct formatter<faio::net::detail::Ipv4Addr> : formatter<string_view> {
  auto format(const faio::net::detail::Ipv4Addr &value,
              format_context &context) const {
    return formatter<string_view>::format(value.to_string(), context);
  }
};
template <>
struct formatter<faio::net::detail::Ipv6Addr> : formatter<string_view> {
  auto format(const faio::net::detail::Ipv6Addr &value,
              format_context &context) const {
    return formatter<string_view>::format(value.to_string(), context);
  }
};
template <>
struct formatter<faio::net::detail::SocketAddr> : formatter<string_view> {
  auto format(const faio::net::detail::SocketAddr &value,
              format_context &context) const {
    return formatter<string_view>::format(value.to_string(), context);
  }
};
} // namespace std
#endif
