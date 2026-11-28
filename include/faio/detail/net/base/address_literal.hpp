/**
 * @file faio/detail/net/base/address_literal.hpp
 * @brief socket 地址字面量的编译期格式校验。
 */
#ifndef FAIO_DETAIL_NET_BASE_ADDRESS_LITERAL_HPP
#define FAIO_DETAIL_NET_BASE_ADDRESS_LITERAL_HPP
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace faio::net::detail {
/** @brief 检查非空十进制文本是否处于指定范围。 */
[[nodiscard]] constexpr auto valid_address_decimal(std::string_view text,
                                                   std::uint32_t limit) noexcept -> bool {
  if (text.empty())
    return false;
  std::uint32_t value{};
  for (const char character : text) {
    if (character < '0' || character > '9')
      return false;
    const auto digit = static_cast<std::uint32_t>(character - '0');
    if (value > (limit - digit) / 10)
      return false;
    value = value * 10 + digit;
  }
  return true;
}

/** @brief 检查四段十进制 IPv4 地址，拒绝前导零。 */
[[nodiscard]] constexpr auto valid_ipv4_literal(std::string_view text) noexcept -> bool {
  for (unsigned group = 0; group != 4; ++group) {
    const auto dot = text.find('.');
    const auto octet = text.substr(0, dot);
    if (octet.size() > 3 || (octet.size() > 1 && octet.front() == '0')
        || !valid_address_decimal(octet, 255))
      return false;
    if (group == 3)
      return dot == std::string_view::npos;
    if (dot == std::string_view::npos)
      return false;
    text.remove_prefix(dot + 1);
  }
  return false;
}

/** @brief 检查 IPv6 分组、单次零压缩和末尾嵌入的 IPv4 地址。 */
[[nodiscard]] constexpr auto valid_ipv6_literal(std::string_view text) noexcept -> bool {
  if (text.empty())
    return false;
  bool compressed = text.starts_with("::");
  std::size_t position = compressed ? 2 : 0;
  unsigned groups{};
  while (position < text.size()) {
    const auto colon = text.find(':', position);
    const auto group = text.substr(position, colon - position);
    if (group.empty())
      return false;
    if (group.find('.') != std::string_view::npos) {
      if (colon != std::string_view::npos || !valid_ipv4_literal(group))
        return false;
      groups += 2;
    } else {
      if (group.size() > 4)
        return false;
      for (const char character : group)
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')
              || (character >= 'A' && character <= 'F')))
          return false;
      ++groups;
    }
    if (groups > 8)
      return false;
    if (colon == std::string_view::npos)
      break;
    position = colon + 1;
    if (position == text.size())
      return false;
    if (text[position] == ':') {
      if (compressed)
        return false;
      compressed = true;
      ++position;
    }
  }
  return compressed ? groups < 8 : groups == 8;
}

/** @brief 检查 IPv6 作用域文本；接口名的存在性留待运行时判断。 */
[[nodiscard]] constexpr auto valid_address_scope(std::string_view scope) noexcept -> bool {
  if (scope.empty())
    return false;
  bool numeric = true;
  for (const char character : scope) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte <= 0x20 || byte == 0x7f || character == '/' || character == ':' || character == '%'
        || character == '[' || character == ']')
      return false;
    if (character < '0' || character > '9')
      numeric = false;
  }
  return !numeric || valid_address_decimal(scope, std::numeric_limits<std::uint32_t>::max());
}

/** @brief 检查带端口的数字端点，IPv6 使用方括号。 */
[[nodiscard]] constexpr auto valid_socket_addr_literal(std::string_view endpoint) noexcept -> bool {
  if (endpoint.find('\0') != std::string_view::npos)
    return false;
  std::string_view host;
  std::string_view service;
  if (endpoint.starts_with('[')) {
    const auto close = endpoint.find(']');
    if (close == std::string_view::npos || close + 1 >= endpoint.size()
        || endpoint[close + 1] != ':')
      return false;
    host = endpoint.substr(1, close - 1);
    service = endpoint.substr(close + 2);
  } else {
    const auto colon = endpoint.find(':');
    if (colon == std::string_view::npos || endpoint.find(':', colon + 1) != std::string_view::npos)
      return false;
    host = endpoint.substr(0, colon);
    service = endpoint.substr(colon + 1);
  }
  if (!valid_address_decimal(service, std::numeric_limits<std::uint16_t>::max()))
    return false;
  if (const auto percent = host.find('%'); percent != std::string_view::npos)
    return valid_address_scope(host.substr(percent + 1))
           && valid_ipv6_literal(host.substr(0, percent));
  return valid_ipv4_literal(host) || valid_ipv6_literal(host);
}

/** @brief 在转换字符串字面量时校验端点格式，保留原始文本供运行时构造地址。 */
class socket_addr_literal {
 public:
  /** @brief 非法字面量使调用处产生编译错误。 */
  template <std::size_t Size>
  consteval socket_addr_literal(const char (&text)[Size]) : value_{text, Size - 1} {
    if (text[Size - 1] != '\0' || !valid_socket_addr_literal(value_))
      throw "socket_addr::from_string expects IPv4:port or [IPv6]:port with port in 0..65535";
  }

  /** @brief 返回经过格式校验的端点文本。 */
  [[nodiscard]] auto value() const noexcept -> std::string_view { return value_; }

 private:
  std::string_view value_;
};
}  // namespace faio::net::detail
#endif
