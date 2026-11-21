/**
 * @file faio/detail/tls/error.hpp
 * @brief 稳定的 TLS 错误分类和 OpenSSL 诊断收集。
 */
#pragma once
#include "faio/detail/common/error.hpp"
#include <openssl/err.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace faio::tls {
/** @brief TLS 错误域的稳定错误码，不以 OpenSSL 原生 reason 代替公共分类。 */
enum class errc : int {
  invalid_config = 1,            /**< 配置内容或安全选项无效。 */
  invalid_server_name = 2,       /**< 服务端 DNS/IP 名称无效。 */
  certificate_verify_failed = 3, /**< 证书链验证失败。 */
  hostname_mismatch = 4,         /**< 证书 SAN 与目标名称不匹配。 */
  handshake_failed = 5,          /**< TLS 握手失败。 */
  protocol_error = 6,            /**< TLS 协议推进失败。 */
  unexpected_eof = 7,            /**< 缺少 close_notify 的底层截断。 */
  unread_application_data = 8,   /**< 关闭时仍有未交付应用明文。 */
  closed = 9,                    /**< 连接已关闭或不能再写入。 */
  unsupported_backend = 10       /**< 底层能力不满足 TLS 驱动约定。 */
};

namespace detail {
/**
 * @brief 立即保存当前线程的 OpenSSL 原生错误队列快照，并清空队列。
 * @return 按队列顺序保存的 reason 列表，后续 OpenSSL 调用不会覆盖该快照。
 */
inline auto take_openssl_reasons() -> std::vector<std::uint64_t> {
  std::vector<std::uint64_t> reasons;
  for (auto reason = ERR_get_error(); reason; reason = ERR_get_error())
    reasons.push_back(reason);
  return reasons;
}

/**
 * @brief 用已保存到快照中的原生原因构造错误，不再读取 OpenSSL 线程局部队列。
 * @param code 稳定的 TLS 错误分类。
 * @param message 拥有型诊断消息，空字符串使用默认说明。
 * @param verify 可选的证书验证返回码。
 * @param reasons 在其他 OpenSSL 查询之前保存的原生错误队列。
 * @return 保留稳定错误码、验证结果与原生原因的新错误值。
 */
inline auto tls_error_with_reasons(errc code,
                                   std::string message,
                                   std::optional<std::int64_t> verify,
                                   std::vector<std::uint64_t> reasons) -> Error {
  auto context = std::make_shared<error_context>();
  context->message = message.empty() ? "TLS error" : std::move(message);
  context->verify_code = verify;
  context->native_reasons = std::move(reasons);
  return Error{static_cast<int>(code), 0, error_domain::tls}.with_context(std::move(context));
}

/**
 * @brief 构造 TLS 错误并一次性取走当前线程的 OpenSSL reason 队列。
 * @param code 稳定的 TLS 错误分类。
 * @param message 用于诊断的消息，空字符串使用默认说明。
 * @param verify 可选的证书验证返回码。
 * @return 保留错误域和诊断上下文的新错误值。
 */
inline Error tls_error(errc code,
                       std::string message = {},
                       std::optional<std::int64_t> verify = {}) {
  return tls_error_with_reasons(code, std::move(message), verify, take_openssl_reasons());
}
}  // namespace detail
}  // namespace faio::tls
