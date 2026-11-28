/**
 * @file faio/detail/tls/detail/bio_engine.hpp
 * @brief 不执行网络 I/O 的有界内存 BIO 引擎及单步 TLS 状态机。
 */
#pragma once
#include "faio/detail/tls/config.hpp"
#include "faio/detail/tls/server_name.hpp"
#include <algorithm>
#include <climits>
#include <cstring>
#include <optional>
#include <span>

namespace faio::tls::detail {
/** @brief 内存 BIO、明文暂存及证书列表的硬上限。 */
struct bio_limits {
  std::size_t inbound_cipher_bytes{64 * 1024};         /**< 入站密文 BIO 的容量上限。 */
  std::size_t outbound_cipher_bytes{64 * 1024};        /**< 出站密文 BIO 的容量上限。 */
  std::size_t plaintext_read_bytes{32 * 1024};         /**< 解密明文暂存区上限。 */
  std::size_t plaintext_write_bytes{16 * 1024};        /**< 单次 SSL 写入的明文上限。 */
  std::size_t wire_chunk_bytes{16 * 1024};             /**< 底层密文 I/O 的分块上限。 */
  std::size_t max_certificate_list_bytes{1024 * 1024}; /**< 握手证书列表的最大编码长度。 */
};
/** @brief 一次 SSL 调用的结果或下一步所需的网络方向。 */
enum class step_status {
  COMPLETE,      /**< 本次操作完成。 */
  WANT_READ,     /**< 需要继续输入网络密文。 */
  WANT_WRITE,    /**< 需要先排空网络密文输出。 */
  PEER_CLOSED,   /**< 已收到对端 close_notify。 */
  LOCAL_SHUTDOWN /**< 已发出本地方向关闭通知。 */
};
/** @brief 需要继续推进的 SSL 操作种类。 */
enum class pending_call {
  HANDSHAKE, /**< 待继续的握手调用。 */
  READ,      /**< 待继续的明文读取。 */
  WRITE,     /**< 地址和长度必须稳定的明文写入。 */
  SHUTDOWN   /**< 待继续的关闭调用。 */
};

/** @brief SSL 单步调用的状态与本次实际完成的明文字节。 */
struct step_result {
  step_status status;            /**< 本次推进状态或所需的网络方向。 */
  std::size_t plaintext_bytes{}; /**< 本次实际完成的明文字节数。 */
};

/** @brief 未完成调用的重试约定，不保存调用方的临时缓冲所有权。 */
struct pending_call_info {
  pending_call call;                /**< 待恢复的 SSL 调用种类。 */
  step_status last_status;          /**< 上一次调用要求的后续动作。 */
  std::size_t stable_buffer_size{}; /**< 写重试必须保持不变的缓冲长度。 */
};

/** @brief 纯内存 TLS 引擎；调用方须串行调用所有方法，不接触 socket 或调度器。 */
class bio_engine {
 public:
  /** @brief 禁止复制构造 TLS 引擎，保持其唯一所有权。 */
  bio_engine(const bio_engine&) = delete;
  /** @brief 禁止复制赋值 TLS 引擎。 */
  auto operator=(const bio_engine&) -> bio_engine& = delete;
  /** @brief 转移源对象的所有权，源对象不再持有该资源。 */
  bio_engine(bio_engine&&) noexcept = default;
  /** @brief 移动赋值；先放弃目标已有的所有权，再接管源对象。 */
  auto operator=(bio_engine&&) noexcept -> bio_engine& = default;
  /** @brief 释放持有的 SSL、BIO 和配置。 */
  ~bio_engine() noexcept = default;

  /**
   * @brief 创建客户端引擎并配置 SAN 验证、DNS SNI 和 ALPN；不执行 I/O。
   * @param config 已验证的客户端安全配置。
   * @param name 规范化的 DNS 或 IP 验证目标。
   * @param limits 内存 BIO 和明文暂存上限。
   * @return 待驱动握手的引擎，或配置/名称设置错误。
   */
  static auto client(client_config config, server_name name, bio_limits limits = {})
      -> expected<bio_engine> {
    auto engine = create(config_access::data(config), false, limits);
    if (!engine)
      return std::unexpected{engine.error()};
    if (name.value().empty())
      return std::unexpected{error{EBADF}};
    auto* ssl = engine->ssl_.get();
    auto* params = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(
        params, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
    const std::string value{name.value()};
    ERR_clear_error();
    if (name.is_ip()) {
      if (X509_VERIFY_PARAM_set1_ip_asc(params, value.c_str()) != 1)
        return std::unexpected{
            tls_error(errc::INVALID_SERVER_NAME, "Cannot configure IP SAN verification")};
    } else if (SSL_set_tlsext_host_name(ssl, value.c_str()) != 1
               || X509_VERIFY_PARAM_set1_host(params, value.c_str(), value.size()) != 1) {
      return std::unexpected{
          tls_error(errc::INVALID_SERVER_NAME, "Cannot configure DNS SAN verification or SNI")};
    }
    const auto& wire = engine->config_->wire;
    if (!wire.empty()
        && SSL_set_alpn_protos(ssl, wire.data(), static_cast<unsigned int>(wire.size())) != 0)
      return std::unexpected{tls_error(errc::INVALID_CONFIG, "Cannot configure client ALPN")};
    SSL_set_connect_state(ssl);
    return engine;
  }

  /**
   * @brief 创建服务端纯内存引擎并设置 accept 握手状态；不执行 I/O。
   * @param config 已验证且具备身份的服务端配置。
   * @param limits 内存 BIO 和明文暂存上限。
   * @return 待驱动握手的引擎，或配置错误。
   */
  static auto server(server_config config, bio_limits limits = {}) -> expected<bio_engine> {
    auto engine = create(config_access::data(config), true, limits);
    if (engine)
      SSL_set_accept_state(engine->ssl_.get());
    return engine;
  }

  /**
   * @brief 向有界 BIO 输入已从网络收取的密文字节。
   * @param bytes 持久网络读取结果的视图。
   * @return 实际写入 BIO 的字节数，0 表示暂时无容量。
   */
  auto feed_ciphertext(std::span<const char> bytes) -> expected<std::size_t> {
    if (bytes.empty() || !input_capacity())
      return std::size_t{};
    const auto length =
        std::min({bytes.size(), input_capacity(), static_cast<std::size_t>(INT_MAX)});
    const auto written = BIO_write(network_.get(), bytes.data(), static_cast<int>(length));
    if (written > 0)
      return static_cast<std::size_t>(written);
    if (BIO_should_retry(network_.get()))
      return std::size_t{};
    return std::unexpected{tls_error(errc::PROTOCOL_ERROR, "Cannot feed TLS ciphertext")};
  }

  /**
   * @brief 从 BIO 提取密文到连接拥有的稳定输出缓冲。
   * @param output 可写密文缓冲。
   * @return 实际提取字节数，0 表示当前无输出。
   */
  auto take_ciphertext(std::span<char> output) -> expected<std::size_t> {
    if (output.empty() || !output_pending())
      return std::size_t{};
    const auto length = std::min(output.size(), static_cast<std::size_t>(INT_MAX));
    const auto read = BIO_read(network_.get(), output.data(), static_cast<int>(length));
    if (read > 0)
      return static_cast<std::size_t>(read);
    if (BIO_should_retry(network_.get()))
      return std::size_t{};
    return std::unexpected{tls_error(errc::PROTOCOL_ERROR, "Cannot extract TLS ciphertext")};
  }

  /**
   * @brief 查询 BIO 当前还能接纳多少入站密文。
   * @return 下一次网络读取允许使用的最大容量。
   */
  auto input_capacity() const noexcept -> std::size_t {
    return BIO_ctrl_get_write_guarantee(network_.get());
  }

  /**
   * @brief 查询尚待从 BIO 提取的出站密文量。
   * @return 当前待发送密文字节数。
   */
  auto output_pending() const noexcept -> std::size_t { return BIO_ctrl_pending(network_.get()); }

  /** @brief 向 BIO 标记底层 EOF，使缺少 close_notify 的结束可被识别。 */
  void notify_transport_eof() noexcept {
    transport_eof_ = true;
    static_cast<void>(BIO_shutdown_wr(network_.get()));
  }

  /**
   * @brief 执行一次 SSL 握手调用，并验证服务端 ALPN 未超出客户端提议范围。
   * @return 完成状态、所需网络方向或 TLS 错误。
   */
  auto handshake_step() -> expected<step_result> {
    ERR_clear_error();
    const int result = SSL_do_handshake(ssl_.get());
    auto step = classify(result, pending_call::HANDSHAKE, 0);
    if (step && step->status == step_status::COMPLETE && !server_) {
      const unsigned char* selected{};
      unsigned int length{};
      SSL_get0_alpn_selected(ssl_.get(), &selected, &length);
      if (length && std::ranges::none_of(config_->alpn, [&](const auto& value) {
            return value.size() == length && std::memcmp(value.data(), selected, length) == 0;
          }))
        return std::unexpected{
            tls_error(errc::HANDSHAKE_FAILED, "Server selected an unoffered ALPN protocol")};
    }
    return step;
  }

  /**
   * @brief 从 SSL 解出明文到稳定暂存区，保留读重试状态。
   * @param stable_staging 由连接拥有且地址稳定的明文暂存区。
   * @return 实际解密字节、所需方向或对端安全关闭。
   */
  auto read_step(std::span<char> stable_staging) -> expected<step_result> {
    if (write_pending_ && write_pending_->last_status == step_status::WANT_WRITE)
      return step_result{step_status::WANT_WRITE, 0};
    if (stable_staging.empty() || stable_staging.size() > limits_.plaintext_read_bytes)
      return std::unexpected{tls_error(errc::INVALID_CONFIG, "Invalid TLS plaintext read staging")};
    std::size_t bytes{};
    ERR_clear_error();
    const int result =
        SSL_read_ex(ssl_.get(), stable_staging.data(), stable_staging.size(), &bytes);
    auto step = classify(result, pending_call::READ, bytes, stable_staging.size());
    if (step && step->status == step_status::COMPLETE)
      read_pending_.reset();
    else if (step
             && (step->status == step_status::WANT_READ || step->status == step_status::WANT_WRITE))
      read_pending_ = pending_call_info{pending_call::READ, step->status, stable_staging.size()};
    return step;
  }

  /**
   * @brief 把稳定明文交给 SSL，并强制重试地址和长度保持不变。
   * @param stable_staging 整个待重试调用期间保持不变的明文视图。
   * @return 实际接纳明文字节、所需方向或错误。
   */
  auto write_step(std::span<const char> stable_staging) -> expected<step_result> {
    if (stable_staging.empty() || stable_staging.size() > limits_.plaintext_write_bytes)
      return std::unexpected{
          tls_error(errc::INVALID_CONFIG, "Invalid TLS plaintext write staging")};
    if (write_pending_
        && (write_address_ != stable_staging.data()
            || write_pending_->stable_buffer_size != stable_staging.size()))
      return std::unexpected{
          tls_error(errc::PROTOCOL_ERROR, "TLS write retry changed its stable buffer")};
    if (read_pending_ && read_pending_->last_status == step_status::WANT_WRITE && !write_pending_)
      return step_result{step_status::WANT_WRITE, 0};
    std::size_t bytes{};
    ERR_clear_error();
    const int result =
        SSL_write_ex(ssl_.get(), stable_staging.data(), stable_staging.size(), &bytes);
    auto step = classify(result, pending_call::WRITE, bytes, stable_staging.size());
    if (step && step->status == step_status::COMPLETE) {
      write_pending_.reset();
      write_address_ = nullptr;
    } else if (step
               && (step->status == step_status::WANT_READ
                   || step->status == step_status::WANT_WRITE)) {
      write_pending_ = pending_call_info{pending_call::WRITE, step->status, stable_staging.size()};
      write_address_ = stable_staging.data();
    }
    return step;
  }

  /**
   * @brief 推进一次双向 TLS 关闭状态；仅在收到对端通知后确认对端关闭。
   * @return 本地已发通知、双方完成、所需方向或错误。
   */
  auto shutdown_step() -> expected<step_result> {
    ERR_clear_error();
    const int result = SSL_shutdown(ssl_.get());
    if (result == 0) {
      pending_ = pending_call_info{pending_call::SHUTDOWN, step_status::LOCAL_SHUTDOWN, 0};
      return step_result{step_status::LOCAL_SHUTDOWN, 0};
    }
    return classify(result, pending_call::SHUTDOWN, 0);
  }

  /**
   * @brief 查询当前必须优先恢复的 SSL 调用约定。
   * @return 写优先、读其次的未完成调用信息。
   */
  auto pending_call() const noexcept -> std::optional<pending_call_info> {
    if (write_pending_)
      return write_pending_;
    if (read_pending_)
      return read_pending_;
    return pending_;
  }

  /**
   * @brief 复制协商结果和对端证书 DER，避免返回对 SSL 内部数据的借用。
   * @return 拥有型连接信息快照。
   */
  auto connection_info() const -> ::faio::tls::connection_info {
    ::faio::tls::connection_info result{
        static_cast<tls_version>(SSL_version(ssl_.get())), {}, {}, {}, {}};
    if (auto* cipher = SSL_get_current_cipher(ssl_.get()))
      result.cipher = SSL_CIPHER_get_name(cipher);
    const unsigned char* selected{};
    unsigned int length{};
    SSL_get0_alpn_selected(ssl_.get(), &selected, &length);
    if (length)
      result.alpn.emplace(reinterpret_cast<const char*>(selected), length);
    certificate_owner leaf{SSL_get1_peer_certificate(ssl_.get())};
    if (leaf) {
      const auto bytes = i2d_X509(leaf.get(), nullptr);
      if (bytes > 0) {
        result.peer_leaf_der.emplace(static_cast<std::size_t>(bytes));
        auto* destination = reinterpret_cast<unsigned char*>(result.peer_leaf_der->data());
        if (i2d_X509(leaf.get(), &destination) != bytes)
          result.peer_leaf_der.reset();
      }
      if (!server_ || config_->auth != client_auth::NONE)
        result.verify_result = SSL_get_verify_result(ssl_.get());
    }
    return result;
  }

 private:
  /**
   * @brief 移入已创建的 SSL 和网络 BIO，并持有配置以维持 SSL_CTX 生命周期。
   * @param config SSL_CTX 与协商策略的共享所有权。
   * @param ssl 当前会话唯一拥有的 SSL。
   * @param network BIO pair 的网络侧端点。
   * @param limits 当前会话采用的容量上限。
   * @param server 是否使用服务端握手状态。
   */
  bio_engine(std::shared_ptr<const config_data> config,
             ssl_owner ssl,
             bio_owner network,
             bio_limits limits,
             bool server)
      : config_(std::move(config)),
        ssl_(std::move(ssl)),
        network_(std::move(network)),
        limits_(limits),
        server_(server) {}

  /**
   * @brief 验证容量并分配 SSL/BIO pair，将内部 BIO 的唯一所有权交给 SSL。
   * @param config 已构建的共享配置，空值返回 EBADF。
   * @param server 连接的客户端或服务端角色。
   * @param limits 非零且受明文/证书硬上限约束的容量参数。
   * @return 纯内存引擎，或配置/分配相关的 TLS 错误。
   */
  static auto create(std::shared_ptr<const config_data> config, bool server, bio_limits limits)
      -> expected<bio_engine> {
    if (!config || !config->context)
      return std::unexpected{error{EBADF}};
    if (!limits.inbound_cipher_bytes || !limits.outbound_cipher_bytes
        || !limits.plaintext_read_bytes || !limits.plaintext_write_bytes
        || limits.plaintext_write_bytes > 16 * 1024 || !limits.wire_chunk_bytes
        || !limits.max_certificate_list_bytes || limits.max_certificate_list_bytes > 1024 * 1024)
      return std::unexpected{tls_error(errc::INVALID_CONFIG, "Invalid bounded BIO limits")};
    ERR_clear_error();
    ssl_owner ssl{SSL_new(config->context.get())};
    BIO* internal_raw{};
    BIO* network_raw{};
    if (!ssl
        || BIO_new_bio_pair(&internal_raw,
                            limits.outbound_cipher_bytes,
                            &network_raw,
                            limits.inbound_cipher_bytes)
               != 1)
      return std::unexpected{
          tls_error(errc::INVALID_CONFIG, "Cannot allocate bounded TLS BIO pair")};
    bio_owner internal{internal_raw}, network{network_raw};
    SSL_set_bio(ssl.get(), internal.get(), internal.get());
    static_cast<void>(internal.release());
    SSL_set_max_cert_list(ssl.get(), static_cast<long>(limits.max_certificate_list_bytes));
    return bio_engine{std::move(config), std::move(ssl), std::move(network), limits, server};
  }

  /**
   * @brief 在同一线程中紧接 SSL 调用，解释其返回值和错误栈。
   * @param result 紧邻 SSL 调用的原始返回值。
   * @param call 当前 SSL 操作种类。
   * @param bytes SSL 实际完成的明文字节。
   * @param size 稳定缓冲总长度。
   * @return 统一的单步状态或带验证信息的 TLS 错误。
   */
  auto classify(int result,
                ::faio::tls::detail::pending_call call,
                std::size_t bytes,
                std::size_t size = 0) -> expected<step_result> {
    if (result == 1) {
      pending_.reset();
      return step_result{step_status::COMPLETE, bytes};
    }

    const auto reason = SSL_get_error(ssl_.get(), result);
    step_status status;
    switch (reason) {
      case SSL_ERROR_WANT_READ:
        status = step_status::WANT_READ;
        break;
      case SSL_ERROR_WANT_WRITE:
        status = step_status::WANT_WRITE;
        break;
      case SSL_ERROR_ZERO_RETURN:
        pending_.reset();
        return step_result{step_status::PEER_CLOSED, 0};
      default: {
        auto reasons = take_openssl_reasons();
        const auto verify = SSL_get_verify_result(ssl_.get());
        if (verify != X509_V_OK) {
          auto code =
              verify == X509_V_ERR_HOSTNAME_MISMATCH || verify == X509_V_ERR_IP_ADDRESS_MISMATCH
                  ? errc::HOSTNAME_MISMATCH
                  : errc::CERTIFICATE_VERIFY_FAILED;
          return std::unexpected{tls_error_with_reasons(
              code, X509_verify_cert_error_string(verify), verify, std::move(reasons))};
        }
        if (transport_eof_
            || (reason == SSL_ERROR_SSL && !reasons.empty()
                && ERR_GET_REASON(static_cast<unsigned long>(reasons.back()))
                       == SSL_R_UNEXPECTED_EOF_WHILE_READING))
          return std::unexpected{tls_error_with_reasons(errc::UNEXPECTED_EOF,
                                                        "Transport EOF without TLS close_notify",
                                                        {},
                                                        std::move(reasons))};
        return std::unexpected{tls_error_with_reasons(
            call == pending_call::HANDSHAKE ? errc::HANDSHAKE_FAILED : errc::PROTOCOL_ERROR,
            "OpenSSL TLS operation failed",
            {},
            std::move(reasons))};
      }
    }
    pending_ = pending_call_info{call, status, size};
    return step_result{status, 0};
  }

  std::shared_ptr<const config_data> config_; /**< 保持 SSL_CTX 和 ALPN 回调参数存活的配置。 */
  ssl_owner ssl_;                             /**< 当前连接唯一拥有的 SSL 对象。 */
  bio_owner network_;                         /**< 由驱动读写的 BIO pair 网络端。 */
  bio_limits limits_;                         /**< 连接的不可变内存与分块上限。 */
  bool server_{};                             /**< 是否采用服务端握手状态。 */
  bool transport_eof_{};                      /**< 底层是否已返回 EOF。 */
  std::optional<pending_call_info> pending_, read_pending_,
      write_pending_;           /**< 需要保留的写入重试状态。 */
  const char* write_address_{}; /**< SSL 写重试要求保持不变的首地址。 */
};
}  // namespace faio::tls::detail
