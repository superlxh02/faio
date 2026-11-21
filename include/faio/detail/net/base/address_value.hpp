/**
 * @file faio/detail/net/base/address_value.hpp
 * @brief IP 与 socket 地址值。
 */
#ifndef FAIO_DETAIL_NET_BASE_ADDRESS_VALUE_HPP
#define FAIO_DETAIL_NET_BASE_ADDRESS_VALUE_HPP
#include "faio/detail/net/platform/select.hpp"

#include "faio/detail/common/error.hpp"
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <variant>

namespace faio::net::detail {
/**  @brief IPv4 值类型；内部保存网络字节序，避免依赖 CPU 大小端。 */
class Ipv4Addr {
 public:
  /** @brief 构造全零的未指定 IPv4 地址。 */
  Ipv4Addr() noexcept = default;

  /** @brief 从网络字节序值或四个字节构造 IPv4 地址。 */
  Ipv4Addr(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) noexcept {
    const std::array bytes{a, b, c, d};
    std::memcpy(&ip_, bytes.data(), bytes.size());
  }

  /** @brief 从网络字节序值或四个字节构造 IPv4 地址。 */
  explicit Ipv4Addr(std::uint32_t network_order) noexcept : ip_{network_order} {}

  /** @brief 返回原生 IP 地址值。 */
  [[nodiscard]] auto addr() const noexcept -> std::uint32_t { return ip_; }

  /** @brief 返回按网络顺序排列的 IPv4 四个字节。 */
  [[nodiscard]] auto octets() const noexcept -> std::array<std::uint8_t, 4> {
    std::array<std::uint8_t, 4> result{};
    std::memcpy(result.data(), &ip_, result.size());
    return result;
  }

  /** @brief 将地址格式化为可读文本。 */
  [[nodiscard]] auto to_string() const -> std::string {
    std::array<char, INET_ADDRSTRLEN> buffer{};
    if (!::inet_ntop(AF_INET, &ip_, buffer.data(), buffer.size()))
      return {};
    return buffer.data();
  }

  /**  @brief 严格解析数字地址；string_view 不要求以 NUL 结尾。 */
  [[nodiscard]] static auto parse(std::string_view text) -> expected<Ipv4Addr> {
    std::uint32_t address{};
    if (text.find('\0') != std::string_view::npos)
      return std::unexpected{make_error(EINVAL)};
    const std::string owned{text};
    if (::inet_pton(AF_INET, owned.c_str(), &address) != 1)
      return std::unexpected{make_error(EINVAL)};
    return Ipv4Addr{address};
  }

  /** @brief 判断 IP 地址是否为全零未指定地址。 */
  [[nodiscard]] auto is_unspecified() const noexcept -> bool { return ip_ == 0; }

  /** @brief 判断 IP 地址是否为回环地址。 */
  [[nodiscard]] auto is_loopback() const noexcept -> bool { return octets()[0] == 127; }

  /** @brief 判断 IP 地址是否属于组播范围。 */
  [[nodiscard]] auto is_multicast() const noexcept -> bool { return (octets()[0] & 0xf0) == 0xe0; }

  /** @brief 比较两个地址值是否相等。 */
  friend auto operator==(const Ipv4Addr&, const Ipv4Addr&) noexcept -> bool = default;

 private:
  std::uint32_t ip_{}; /**< 拥有的 IP 地址值，按所属地址类型保存其字节序。 */
};

/**  @brief IPv6 值类型，使用标准 s6_addr 而非 Linux 内部 union 字段。 */
class Ipv6Addr {
 public:
  /** @brief 构造全零的未指定 IPv6 地址。 */
  Ipv6Addr() noexcept = default;

  /** @brief 从原生地址或八段十六位值构造 IPv6 地址。 */
  Ipv6Addr(std::uint16_t a,
           std::uint16_t b,
           std::uint16_t c,
           std::uint16_t d,
           std::uint16_t e,
           std::uint16_t f,
           std::uint16_t g,
           std::uint16_t h) noexcept {
    const std::array<std::uint16_t, 8> words{
        htons(a), htons(b), htons(c), htons(d), htons(e), htons(f), htons(g), htons(h)};
    std::memcpy(ip_.s6_addr, words.data(), sizeof(ip_));
  }

  /** @brief 从原生地址或八段十六位值构造 IPv6 地址。 */
  explicit Ipv6Addr(in6_addr ip) noexcept : ip_{ip} {}

  /** @brief 返回原生 IP 地址值。 */
  [[nodiscard]] auto addr() const noexcept -> const in6_addr& { return ip_; }

  /** @brief 将地址格式化为可读文本。 */
  [[nodiscard]] auto to_string() const -> std::string {
    std::array<char, INET6_ADDRSTRLEN> buffer{};
    if (!::inet_ntop(AF_INET6, &ip_, buffer.data(), buffer.size()))
      return {};
    return buffer.data();
  }

  /** @brief 解析地址文本，非法格式返回错误。 */
  [[nodiscard]] static auto parse(std::string_view text) -> expected<Ipv6Addr> {
    in6_addr address{};
    if (text.find('\0') != std::string_view::npos)
      return std::unexpected{make_error(EINVAL)};
    const std::string owned{text};
    if (::inet_pton(AF_INET6, owned.c_str(), &address) != 1)
      return std::unexpected{make_error(EINVAL)};
    return Ipv6Addr{address};
  }

  /** @brief 判断 IP 地址是否为全零未指定地址。 */
  [[nodiscard]] auto is_unspecified() const noexcept -> bool {
    return IN6_IS_ADDR_UNSPECIFIED(&ip_);
  }

  /** @brief 判断 IP 地址是否为回环地址。 */
  [[nodiscard]] auto is_loopback() const noexcept -> bool { return IN6_IS_ADDR_LOOPBACK(&ip_); }

  /** @brief 判断 IP 地址是否属于组播范围。 */
  [[nodiscard]] auto is_multicast() const noexcept -> bool { return IN6_IS_ADDR_MULTICAST(&ip_); }

  /** @brief 比较两个地址值是否相等。 */
  friend auto operator==(const Ipv6Addr& a, const Ipv6Addr& b) noexcept -> bool {
    return std::memcmp(&a.ip_, &b.ip_, sizeof(in6_addr)) == 0;
  }

 private:
  in6_addr ip_{}; /**< 拥有的 IP 地址值，按所属地址类型保存其字节序。 */
};

/**  @brief 拥有型 IP 端点，完整保存 IPv6 scope id 与 flow info。 */
class SocketAddr {
 public:
  /** @brief 复制 IP 和端口或原生套接字地址，构造端点。 */
  SocketAddr() noexcept = default;

  /** @brief 复制 IP 和端口或原生套接字地址，构造端点。 */
  SocketAddr(const struct sockaddr* address, std::size_t length) noexcept {
    if (address
        && length >= offsetof(struct sockaddr, sa_family) + sizeof(decltype(::sockaddr{}.sa_family))
        && length <= sizeof(storage_)
        && ((address->sa_family == AF_INET && length >= sizeof(sockaddr_in))
            || (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6))))
      std::memcpy(&storage_, address, length);
  }

  /** @brief 复制 IP 和端口或原生套接字地址，构造端点。 */
  SocketAddr(Ipv4Addr ip, std::uint16_t port) noexcept {
    auto* address = reinterpret_cast<sockaddr_in*>(&storage_);
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    address->sin_addr.s_addr = ip.addr();
    set_native_address_length(*address);
  }

  /** @brief 复制 IP 和端口或原生套接字地址，构造端点。 */
  SocketAddr(const Ipv6Addr& ip, std::uint16_t port, std::uint32_t scope = 0) noexcept {
    auto* address = reinterpret_cast<sockaddr_in6*>(&storage_);
    address->sin6_family = AF_INET6;
    address->sin6_port = htons(port);
    address->sin6_addr = ip.addr();
    address->sin6_scope_id = scope;
    set_native_address_length(*address);
  }

  /** @brief 返回 socket 地址中的 IPv4 或 IPv6 地址值。 */
  [[nodiscard]] auto ip() const noexcept -> std::variant<Ipv4Addr, Ipv6Addr> {
    if (is_ipv4())
      return Ipv4Addr{v4().sin_addr.s_addr};
    return Ipv6Addr{v6().sin6_addr};
  }

  /** @brief 替换 socket 地址中的 IP 地址。 */
  void set_ip(Ipv4Addr ip) noexcept { *this = SocketAddr{ip, port()}; }

  /** @brief 替换 socket 地址中的 IP 地址。 */
  void set_ip(const Ipv6Addr& ip) noexcept { *this = SocketAddr{ip, port(), scope_id()}; }

  /** @brief 返回主机字节序的端口号。 */
  [[nodiscard]] auto port() const noexcept -> std::uint16_t {
    return is_ipv4() ? ntohs(v4().sin_port) : ntohs(v6().sin6_port);
  }

  /** @brief 设置主机字节序端口号并写入原生地址。 */
  void set_port(std::uint16_t value) noexcept {
    if (is_ipv4())
      reinterpret_cast<sockaddr_in*>(&storage_)->sin_port = htons(value);
    else
      reinterpret_cast<sockaddr_in6*>(&storage_)->sin6_port = htons(value);
  }

  /** @brief 返回 IPv6 地址的作用域接口标识。 */
  [[nodiscard]] auto scope_id() const noexcept -> std::uint32_t {
    return is_ipv6() ? v6().sin6_scope_id : 0;
  }

  /** @brief 设置 IPv6 地址的作用域接口标识。 */
  void set_scope_id(std::uint32_t value) noexcept {
    if (is_ipv6())
      reinterpret_cast<sockaddr_in6*>(&storage_)->sin6_scope_id = value;
  }

  /** @brief 将地址格式化为可读文本。 */
  [[nodiscard]] auto to_string() const -> std::string {
    if (is_ipv4())
      return Ipv4Addr{v4().sin_addr.s_addr}.to_string() + ":" + std::to_string(port());
    if (is_ipv6()) {
      auto host = Ipv6Addr{v6().sin6_addr}.to_string();
      if (scope_id())
        host += "%" + std::to_string(scope_id());
      return "[" + host + "]:" + std::to_string(port());
    }
    return {};
  }

  /** @brief 判断当前 socket 地址是否使用 IPv4。 */
  [[nodiscard]] auto is_ipv4() const noexcept -> bool { return family() == AF_INET; }

  /** @brief 判断当前 socket 地址是否使用 IPv6。 */
  [[nodiscard]] auto is_ipv6() const noexcept -> bool { return family() == AF_INET6; }

  /** @brief 返回地址或 socket 所属的原生地址族。 */
  [[nodiscard]] auto family() const noexcept -> int { return storage_.ss_family; }

  /** @brief 借用原生套接字地址存储，有效期由当前对象决定。 */
  [[nodiscard]] auto sockaddr() const noexcept -> const struct sockaddr* {
    return reinterpret_cast<const struct sockaddr*>(&storage_);
  }

  /** @brief 借用原生套接字地址存储，有效期由当前对象决定。 */
  [[nodiscard]] auto sockaddr() noexcept -> struct sockaddr* {
    return reinterpret_cast<struct sockaddr*>(&storage_);
  }

  /** @brief 返回传给原生 socket 接口的有效地址字节数。 */
  [[nodiscard]] auto length() const noexcept -> socklen_t {
    return is_ipv4()   ? sizeof(sockaddr_in)
           : is_ipv6() ? sizeof(sockaddr_in6)
                       : sizeof(sockaddr_storage);
  }

  /** @brief 返回原生 socket 地址存储的字节容量。 */
  [[nodiscard]] static constexpr auto capacity() noexcept -> socklen_t {
    return sizeof(sockaddr_storage);
  }

  /**  @brief 数字地址解析；支持 IPv6 的 %数字 / %接口名作用域，不执行 DNS。 */
  [[nodiscard]] static auto numeric_parse(std::string_view host, std::uint16_t port)
      -> expected<SocketAddr> {
    if (host.find('\0') != std::string_view::npos)
      return std::unexpected{make_error(EINVAL)};
    if (auto ip = Ipv4Addr::parse(host))
      return SocketAddr{*ip, port};
    std::uint32_t scope{};
    if (const auto pos = host.find('%'); pos != std::string_view::npos) {
      const auto suffix = host.substr(pos + 1);
      auto [end, error] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), scope);
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

  /**
   *  @brief 同步纯数字解析；域名应使用 lookup_host，不在 worker 中执行阻塞
   * DNS。
   */
  [[nodiscard]] static auto parse(std::string_view host, std::uint16_t port)
      -> expected<SocketAddr> {
    return numeric_parse(host, port);
  }

  /**  @brief 解析包含端口的数字端点，IPv6 要求 [地址]:端口 格式。 */
  [[nodiscard]] static auto parse(std::string_view endpoint) -> expected<SocketAddr> {
    std::string_view host;
    std::string_view service;
    if (endpoint.starts_with('[')) {
      const auto close = endpoint.find(']');
      if (close == std::string_view::npos || close + 1 >= endpoint.size()
          || endpoint[close + 1] != ':')
        return std::unexpected{make_error(EINVAL)};
      host = endpoint.substr(1, close - 1);
      service = endpoint.substr(close + 2);
    } else {
      const auto colon = endpoint.find(':');
      if (colon == std::string_view::npos
          || endpoint.find(':', colon + 1) != std::string_view::npos)
        return std::unexpected{make_error(EINVAL)};
      host = endpoint.substr(0, colon);
      service = endpoint.substr(colon + 1);
    }
    unsigned port{};
    const auto [end, error] =
        std::from_chars(service.data(), service.data() + service.size(), port);
    if (service.empty() || error != std::errc{} || end != service.data() + service.size()
        || port > 65535)
      return std::unexpected{make_error(EINVAL)};
    return numeric_parse(host, static_cast<std::uint16_t>(port));
  }

  /** @brief 比较两个地址值是否相等。 */
  friend auto operator==(const SocketAddr& a, const SocketAddr& b) noexcept -> bool {
    if (a.family() != b.family() || a.port() != b.port())
      return false;
    if (a.is_ipv4())
      return a.v4().sin_addr.s_addr == b.v4().sin_addr.s_addr;
    if (a.is_ipv6())
      return a.scope_id() == b.scope_id()
             && std::memcmp(&a.v6().sin6_addr, &b.v6().sin6_addr, sizeof(in6_addr)) == 0;
    return true;
  }

 private:
  /** @brief 借用当前地址的 IPv4 原生存储。 */
  [[nodiscard]] auto v4() const noexcept -> const sockaddr_in& {
    return *reinterpret_cast<const sockaddr_in*>(&storage_);
  }

  /** @brief 借用当前地址的 IPv6 原生存储。 */
  [[nodiscard]] auto v6() const noexcept -> const sockaddr_in6& {
    return *reinterpret_cast<const sockaddr_in6*>(&storage_);
  }

  sockaddr_storage storage_{}; /**< 保存完整原生地址的存储区，不借用调用方临时地址。 */
};
}  // namespace faio::net::detail

namespace std {
/** @brief IPv4 地址的标准格式化适配。 */
template <>
struct formatter<faio::net::detail::Ipv4Addr> : formatter<string_view> {
  /** @brief 将地址的文本表示写入格式化上下文。 */
  auto format(const faio::net::detail::Ipv4Addr& value, format_context& context) const {
    return formatter<string_view>::format(value.to_string(), context);
  }
};

/** @brief IPv6 地址的标准格式化适配。 */
template <>
struct formatter<faio::net::detail::Ipv6Addr> : formatter<string_view> {
  /** @brief 将地址的文本表示写入格式化上下文。 */
  auto format(const faio::net::detail::Ipv6Addr& value, format_context& context) const {
    return formatter<string_view>::format(value.to_string(), context);
  }
};

/** @brief IP 端点地址的标准格式化适配。 */
template <>
struct formatter<faio::net::detail::SocketAddr> : formatter<string_view> {
  /** @brief 将地址的文本表示写入格式化上下文。 */
  auto format(const faio::net::detail::SocketAddr& value, format_context& context) const {
    return formatter<string_view>::format(value.to_string(), context);
  }
};
}  // namespace std
#endif
