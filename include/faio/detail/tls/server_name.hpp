/**
 * @file faio/detail/tls/server_name.hpp
 * @brief 规范化 DNS 名称与 IP 字面量，供 SNI 和证书 SAN 验证使用。
 */
#pragma once
#include "faio/detail/tls/error.hpp"
#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <array>
#include <string>
#include <string_view>

namespace faio::tls {
/** @brief 经校验的服务端名称，区分 DNS 与 IP 身份验证路径。 */
class ServerName {
 public:
  /** @brief 禁止默认构造尚未经验证的服务端名称。 */
  ServerName() = delete;
  /** @brief 复制规范化名称文本和 DNS/IP 分类。 */
  ServerName(const ServerName&) = default;
  /** @brief 用另一对象的名称文本和 DNS/IP 分类替换当前值。 */
  auto operator=(const ServerName&) -> ServerName& = default;
  /** @brief 移动构造名称文本及 DNS/IP 分类。 */
  ServerName(ServerName&&) noexcept = default;
  /** @brief 移动赋值；先放弃目标已有的所有权，再接管源对象。 */
  auto operator=(ServerName&&) noexcept -> ServerName& = default;
  /** @brief 释放持有的名称文本。 */
  ~ServerName() noexcept = default;

  /**
   * @brief 规范化并验证 DNS 名称或不带 zone 的 IP 字面量。
   * @param name 不含端口的目标名称。
   * @return 已校验名称，或 invalid_server_name。
   */
  static auto parse(std::string name) -> expected<ServerName> {
    auto fail = [] {
      return std::unexpected{
          detail::tls_error(errc::invalid_server_name, "Invalid TLS DNS name or IP address")};
    };
    if (name.empty() || name.find('\0') != std::string::npos || name.find('%') != std::string::npos)
      return fail();
    std::array<unsigned char, 16> address{};
    std::array<char, INET6_ADDRSTRLEN> normalized{};
    for (auto family : {AF_INET, AF_INET6}) {
      if (inet_pton(family, name.c_str(), address.data()) == 1) {
        if (!inet_ntop(family, address.data(), normalized.data(), normalized.size()))
          return fail();
        return ServerName{normalized.data(), true};
      }
    }
    if (name.back() == '.')
      name.pop_back();
    if (name.empty() || name.size() > 253)
      return fail();
    std::size_t label{};
    for (std::size_t i = 0; i < name.size(); ++i) {
      auto& c = name[i];
      if (c >= 'A' && c <= 'Z')
        c = static_cast<char>(c + ('a' - 'A'));
      if (c == '.') {
        if (!label || name[i - 1] == '-')
          return fail();
        label = 0;
        continue;
      }
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
        return fail();
      if ((!label && c == '-') || ++label > 63)
        return fail();
    }
    if (!label || name.back() == '-')
      return fail();

    if (name.find('.') != std::string::npos
        && name.find_first_not_of("0123456789.") == std::string::npos)
      return fail();
    return ServerName{std::move(name), false};
  }

  /**
   * @brief 借用规范化后的名称文本。
   * @return 随本对象有效的只读名称视图。
   */
  auto value() const noexcept -> std::string_view { return name_; }

  /**
   * @brief 判断该名称是否走 IP SAN 验证路径。
   * @return IP 字面量返回 true，DNS 名称返回 false。
   */
  auto is_ip() const noexcept -> bool { return ip_; }

 private:
  /**
   * @brief 保存已经由 parse 校验并规范化的名称和分类。
   * @param name 已规范化且不含端口的名称文本。
   * @param ip 是否采用 IP SAN 验证。
   */
  explicit ServerName(std::string name, bool ip) : name_(std::move(name)), ip_(ip) {}

  std::string name_; /**< 规范化后的 DNS 或 IP 文本。 */
  bool ip_{};        /**< 是否采用 IP SAN 校验。 */
};
}  // namespace faio::tls
