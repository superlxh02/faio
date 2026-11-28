/**
 * @file faio/detail/tls/connection_info.hpp
 * @brief TLS 协议选项及拥有型连接信息快照。
 */
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace faio::tls {
/** @brief 允许协商的 TLS 版本，取值与 OpenSSL 协议版本常量一致。 */
enum class tls_version : std::uint16_t {
  V1_2 = 0x0303, /**< TLS 1.2 协议版本。 */
  V1_3 = 0x0304  /**< TLS 1.3 协议版本。 */
};
/** @brief 服务端验证客户端证书的策略。 */
enum class client_auth {
  NONE,                 /**< 不请求客户端证书。 */
  OPTIONAL_CERTIFICATE, /**< 如客户端提供证书则验证，否则允许继续。 */
  REQUIRED              /**< 必须提供且通过验证的客户端证书。 */
};

/** @brief 连接协商结果的拥有型快照，独立于 SSL 对象生命周期。 */
struct connection_info {
  tls_version version;                            /**< 实际协商的 TLS 版本。 */
  std::string cipher;                             /**< 协商密码套件名称的拥有型副本。 */
  std::optional<std::string> alpn;                /**< 协商出的 ALPN；未协商时为空。 */
  std::optional<std::int64_t> verify_result;      /**< 实际验证对端证书时保存的验证码。 */
  std::optional<std::vector<char>> peer_leaf_der; /**< 对端叶证书 DER 的拥有型副本。 */
};
}  // namespace faio::tls
