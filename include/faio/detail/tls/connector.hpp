/**
 * @file faio/detail/tls/connector.hpp
 * @brief 在已有异步传输上启动经过身份验证的客户端握手。
 */
#pragma once
#include "faio/detail/io/stream/operation.hpp"
#include "faio/detail/tls/stream.hpp"

namespace faio::tls {
namespace detail {
/** @brief 创建客户端 TLS 引擎并在给定传输上完成握手。 */
template <protocol_transport S>
auto connect_impl(client_config config, server_name name, S stream, io::operation_options options)
    -> task<expected<tls_stream<S>>> {
  auto engine = bio_engine::client(std::move(config), std::move(name));
  if (!engine)
    co_return std::unexpected{engine.error()};
  auto state = tls_state<S>::create(std::move(stream), std::move(*engine));
  if (!state)
    co_return std::unexpected{state.error()};
  co_return co_await tls_handshake(std::move(*state), std::move(options));
}
}  // namespace detail

/** @brief 可复制的客户端握手器，不负责创建底层连接。 */
class tls_connector {
 public:
  /** @brief 禁止构造缺少客户端配置的握手器。 */
  tls_connector() = delete;

  /**
   * @brief 持有可复用的不可变客户端配置，不创建传输或执行握手。
   * @param config 已构建的安全配置。
   */
  explicit tls_connector(client_config config) : config_(std::move(config)) {}

  /** @brief 复制可共享配置的所有权。 */
  tls_connector(const tls_connector&) noexcept = default;
  /** @brief 复制共享配置所有权，不复制底层证书或 SSL_CTX。 */
  auto operator=(const tls_connector&) noexcept -> tls_connector& = default;
  /** @brief 转移源对象的所有权，源对象不再持有该资源。 */
  tls_connector(tls_connector&&) noexcept = default;
  /** @brief 移动赋值；先放弃目标已有的所有权，再接管源对象。 */
  auto operator=(tls_connector&&) noexcept -> tls_connector& = default;
  /** @brief 释放持有的客户端共享配置。 */
  ~tls_connector() noexcept = default;

  /**
   * @brief 返回共享持有的当前不可变配置。
   * @return 可独立持有的配置值。
   */
  auto config() const noexcept -> client_config { return config_; }

  /**
   * @brief 校验新的 ALPN 列表并派生客户端握手器，保持原握手器不变。
   * @param protocols 派生连接将提出的协议列表。
   * @return 复用 SSL_CTX 的新握手器，或移动后对象/ALPN 校验错误。
   */
  auto with_alpn(std::vector<std::string> protocols) const -> expected<tls_connector> {
    const auto& original = detail::config_access::data(config_);
    if (!original)
      return std::unexpected{error{EBADF}};
    auto wire = detail::alpn_wire(protocols);
    if (!wire)
      return std::unexpected{wire.error()};
    auto data = std::make_shared<detail::config_data>(*original);
    data->alpn = std::move(protocols);
    data->wire = std::move(*wire);
    return tls_connector{detail::config_access::client(std::move(data))};
  }

  /** @brief 在已有异步传输上完成服务端身份验证与客户端握手。 */
  template <detail::protocol_transport S>
  auto connect(server_name name, S stream, io::operation_options options = {}) const
      -> task<expected<tls_stream<S>>> {
    return detail::connect_impl(
        config_,
        std::move(name),
        std::move(stream),
        io::detail::with_default_deadline(std::move(options), std::chrono::seconds(10)));
  }

 private:
  client_config config_; /**< 客户端握手使用的不可变配置。 */
};
}  // namespace faio::tls
