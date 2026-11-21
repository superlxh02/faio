/**
 * @file faio/detail/tls/acceptor.hpp
 * @brief 在已有异步传输上启动服务端握手。
 */
#pragma once
#include "faio/detail/io/stream/operation.hpp"
#include "faio/detail/tls/stream.hpp"

namespace faio::tls {
namespace detail {
/** @brief 创建服务端 TLS 引擎并在给定传输上完成握手。 */
template <protocol_transport S>
auto accept_impl(ServerConfig config, S stream, io::OperationOptions options)
    -> task<expected<TlsStream<S>>> {
  auto engine = BioEngine::server(std::move(config));
  if (!engine)
    co_return std::unexpected{engine.error()};
  auto state = TlsState<S>::create(std::move(stream), std::move(*engine));
  if (!state)
    co_return std::unexpected{state.error()};
  co_return co_await tls_handshake(std::move(*state), std::move(options));
}
}  // namespace detail

/** @brief 可复制的服务端握手器，可为多条连接复用配置。 */
class TlsAcceptor {
 public:
  /** @brief 禁止构造缺少服务端配置的握手器。 */
  TlsAcceptor() = delete;

  /**
   * @brief 持有可复用的不可变服务端配置，不创建传输或执行握手。
   * @param config 已构建的安全配置。
   */
  explicit TlsAcceptor(ServerConfig config) : config_(std::move(config)) {}

  /** @brief 复制可共享配置的所有权。 */
  TlsAcceptor(const TlsAcceptor&) noexcept = default;
  /** @brief 复制共享配置所有权，不复制底层证书或 SSL_CTX。 */
  auto operator=(const TlsAcceptor&) noexcept -> TlsAcceptor& = default;
  /** @brief 转移源对象的所有权，源对象不再持有该资源。 */
  TlsAcceptor(TlsAcceptor&&) noexcept = default;
  /** @brief 移动赋值；先放弃目标已有的所有权，再接管源对象。 */
  auto operator=(TlsAcceptor&&) noexcept -> TlsAcceptor& = default;
  /** @brief 释放持有的服务端共享配置。 */
  ~TlsAcceptor() noexcept = default;

  /**
   * @brief 返回共享持有的当前不可变配置。
   * @return 可独立持有的配置值。
   */
  auto config() const noexcept -> ServerConfig { return config_; }

  /** @brief 在已有异步传输上完成服务端 TLS 握手。 */
  template <detail::protocol_transport S>
  auto accept(S stream, io::OperationOptions options = {}) const -> task<expected<TlsStream<S>>> {
    return detail::accept_impl(
        config_,
        std::move(stream),
        io::detail::with_default_deadline(std::move(options), std::chrono::seconds(10)));
  }

 private:
  ServerConfig config_; /**< 服务端握手使用的不可变配置。 */
};
}  // namespace faio::tls
