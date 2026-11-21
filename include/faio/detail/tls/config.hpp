/**
 * @file faio/detail/tls/config.hpp
 * @brief 同步加载身份与信任材料，构建可共享的不可变 TLS 安全配置。
 */
#pragma once
#include "faio/detail/tls/connection_info.hpp"
#include "faio/detail/tls/detail/openssl_raii.hpp"
#include "faio/detail/tls/error.hpp"
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace faio::tls {
namespace detail {
/** @brief 供连接实现访问私有配置存储的受控桥接。 */
struct ConfigAccess;

/** @brief 共享身份的实际存储，按叶证书到签发者顺序保存证书链。 */
struct IdentityData {
  std::vector<CertificateOwner> certificates; /**< 按叶证书优先顺序保存的证书链。 */
  PrivateKeyOwner key;                        /**< 与叶证书匹配的私钥。 */
};

/**
 * @brief 同步读取初始化配置文件，并保留打开或读取失败的系统错误。
 * @param path 配置文件路径。
 * @return 完整文件内容，或系统 I/O 错误。
 */
inline auto read_config_file(const std::filesystem::path& path) -> expected<std::string> {
  errno = 0;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::unexpected{Error{errno ? errno : EIO}};
  std::string text{std::istreambuf_iterator<char>(input), {}};
  if (input.bad())
    return std::unexpected{Error{EIO}};
  return text;
}

/**
 * @brief 检查 PEM 边界之外是否仅含允许的空白。
 * @param text 待检查的文本片段。
 * @return 全部字符均为空白时返回 true。
 */
inline bool whitespace(std::string_view text) {
  return std::ranges::all_of(
      text, [](unsigned char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; });
}

/**
 * @brief 按原顺序解析有界 PEM 证书列表，拒绝混入其他内容。
 * @param pem 总长度不超过 1 MiB 的证书文本。
 * @return 拥有各张 X509 证书的列表，或配置错误。
 */
inline auto certificates_from_pem(std::string_view pem) -> expected<std::vector<CertificateOwner>> {
  constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----",
                             end = "-----END CERTIFICATE-----";
  if (pem.empty() || pem.size() > 1024 * 1024)
    return std::unexpected{
        tls_error(errc::invalid_config, "Certificate list is empty or exceeds 1 MiB")};
  std::vector<CertificateOwner> result;
  std::size_t position{};
  while (position < pem.size()) {
    auto start = pem.find(begin, position);
    if (start == std::string_view::npos) {
      if (!whitespace(pem.substr(position)))
        return std::unexpected{tls_error(errc::invalid_config, "Unexpected PEM data")};
      break;
    }
    if (!whitespace(pem.substr(position, start - position)))
      return std::unexpected{tls_error(errc::invalid_config, "Unexpected PEM data")};
    auto stop = pem.find(end, start + begin.size());
    if (stop == std::string_view::npos)
      return std::unexpected{tls_error(errc::invalid_config, "Unterminated certificate PEM")};
    stop += end.size();
    BioOwner bio{BIO_new_mem_buf(pem.data() + start, static_cast<int>(stop - start))};
    ERR_clear_error();
    CertificateOwner certificate{bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)
                                     : nullptr};
    if (!certificate)
      return std::unexpected{tls_error(errc::invalid_config, "Invalid certificate PEM")};
    result.push_back(std::move(certificate));
    position = stop;
  }
  if (result.empty())
    return std::unexpected{tls_error(errc::invalid_config, "PEM contains no certificates")};
  return result;
}

/**
 * @brief 禁止读取加密私钥时交互式请求口令。
 * @return 始终返回 0，表示不提供口令。
 */
inline int no_password(char*, int, int, void*) {
  return 0;
}

/**
 * @brief 验证 ALPN 长度和唯一性，并编码为长度前缀格式。
 * @param protocols 按优先级排列的协议名称。
 * @return 可交给 OpenSSL 的传输格式字节，或配置错误。
 */
inline auto alpn_wire(std::span<const std::string> protocols)
    -> expected<std::vector<unsigned char>> {
  std::vector<unsigned char> result;
  for (std::size_t i = 0; i < protocols.size(); ++i) {
    const auto& value = protocols[i];
    if (value.empty() || value.size() > 255 || value.size() + 1 > 65535 - result.size())
      return std::unexpected{tls_error(errc::invalid_config, "Invalid ALPN protocol length")};
    if (std::find(protocols.begin(), protocols.begin() + static_cast<std::ptrdiff_t>(i), value)
        != protocols.begin() + static_cast<std::ptrdiff_t>(i))
      return std::unexpected{tls_error(errc::invalid_config, "Duplicate ALPN protocol")};
    result.push_back(static_cast<unsigned char>(value.size()));
    result.insert(result.end(), value.begin(), value.end());
  }
  return result;
}
}  // namespace detail

/** @brief 可复制共享的证书信任根集合。 */
class TrustStore {
 public:
  /** @brief 禁止默认构造缺少必要材料的信任根集合。 */
  TrustStore() = delete;
  /** @brief 复制信任根集合的共享所有权。 */
  TrustStore(const TrustStore&) noexcept = default;
  /** @brief 复制赋值信任根集合，不复制底层安全材料。 */
  auto operator=(const TrustStore&) noexcept -> TrustStore& = default;
  /** @brief 转移信任根集合的所有权，源对象不再持有该资源。 */
  TrustStore(TrustStore&&) noexcept = default;
  /** @brief 移动赋值；先放弃目标已有的所有权，再接管源对象。 */
  auto operator=(TrustStore&&) noexcept -> TrustStore& = default;
  /** @brief 释放持有的信任根集合。 */
  ~TrustStore() noexcept = default;

  /**
   * @brief 从 PEM 文本构造并校验共享的信任或身份材料。
   * @param pem 信任证书 PEM。
   * @return 验证成功的材料对象，或配置错误。
   */
  static auto from_pem(std::string pem) -> expected<TrustStore> {
    auto certificates = detail::certificates_from_pem(pem);
    if (!certificates)
      return std::unexpected{certificates.error()};
    detail::CertificateStoreOwner store{X509_STORE_new()};
    if (!store)
      return std::unexpected{
          detail::tls_error(errc::invalid_config, "Cannot allocate trust store")};
    for (const auto& certificate : *certificates) {
      ERR_clear_error();
      if (X509_STORE_add_cert(store.get(), certificate.get()) != 1)
        return std::unexpected{
            detail::tls_error(errc::invalid_config, "Cannot add trust certificate")};
    }
    return TrustStore{std::shared_ptr<X509_STORE>(store.release(), X509_STORE_free)};
  }

  /**
   * @brief 同步读取 PEM 文件并构造信任根集合。
   * @param path CA 证书文件路径。
   * @return 可共享信任根，或读取/配置错误。
   */
  static auto from_pem_file(std::filesystem::path path) -> expected<TrustStore> {
    auto pem = detail::read_config_file(path);
    if (!pem)
      return std::unexpected{pem.error()};
    return from_pem(std::move(*pem));
  }

  /**
   * @brief 加载 OpenSSL 配置的默认 CA 路径。
   * @return 信任根集合；不代表平台原生信任策略。
   */
  static auto from_openssl_defaults() -> expected<TrustStore> {
    detail::CertificateStoreOwner store{X509_STORE_new()};
    ERR_clear_error();
    if (!store || X509_STORE_set_default_paths(store.get()) != 1)
      return std::unexpected{
          detail::tls_error(errc::invalid_config, "Cannot load OpenSSL default CA paths")};
    return TrustStore{std::shared_ptr<X509_STORE>(store.release(), X509_STORE_free)};
  }

 private:
  /** @brief 允许内部桥接访问私有共享存储，不向公开 API 暴露原生所有权。 */
  friend struct detail::ConfigAccess;

  /** @brief 保存已装载证书的共享 X509_STORE，不再重复解析 PEM。 */
  explicit TrustStore(std::shared_ptr<X509_STORE> store) : store_(std::move(store)) {}

  std::shared_ptr<X509_STORE> store_; /**< 可复制共享的 OpenSSL 信任存储。 */
};

/** @brief 证书链和匹配私钥组成的可共享身份。 */
class Identity {
 public:
  /** @brief 禁止默认构造缺少必要材料的证书身份。 */
  Identity() = delete;
  /** @brief 复制证书身份的共享所有权。 */
  Identity(const Identity&) noexcept = default;
  /** @brief 复制赋值证书身份，不复制底层安全材料。 */
  auto operator=(const Identity&) noexcept -> Identity& = default;
  /** @brief 转移证书身份的所有权，源对象不再持有该资源。 */
  Identity(Identity&&) noexcept = default;
  /** @brief 移动赋值；先放弃目标已有的所有权，再接管源对象。 */
  auto operator=(Identity&&) noexcept -> Identity& = default;
  /** @brief 释放持有的证书身份。 */
  ~Identity() noexcept = default;

  /**
   * @brief 从 PEM 文本构造并校验共享的信任或身份材料。
   * @param chain_pem 叶证书优先的证书链 PEM。
   * @param key_pem 与叶证书匹配的未加密私钥 PEM。
   * @return 验证成功的材料对象，或配置错误。
   */
  static auto from_pem(std::string chain_pem, std::string key_pem) -> expected<Identity> {
    auto certificates = detail::certificates_from_pem(chain_pem);
    if (!certificates)
      return std::unexpected{certificates.error()};
    if (key_pem.empty() || key_pem.size() > 1024 * 1024)
      return std::unexpected{
          detail::tls_error(errc::invalid_config, "Private key is empty or exceeds 1 MiB")};
    detail::BioOwner bio{BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size()))};
    ERR_clear_error();
    detail::PrivateKeyOwner key{
        bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, detail::no_password, nullptr) : nullptr};
    if (!key || X509_check_private_key(certificates->front().get(), key.get()) != 1)
      return std::unexpected{detail::tls_error(
          errc::invalid_config, "Private key is invalid or does not match the leaf certificate")};
    for (std::size_t i = 1; i < certificates->size(); ++i) {
      auto* child = (*certificates)[i - 1].get();
      auto* issuer = (*certificates)[i].get();
      if (X509_check_issued(issuer, child) != X509_V_OK
          || X509_verify(child, X509_get0_pubkey(issuer)) != 1)
        return std::unexpected{
            detail::tls_error(errc::invalid_config,
                              "Certificate chain is not leaf-first or has an invalid signature")};
    }
    auto data = std::make_shared<detail::IdentityData>();
    data->certificates = std::move(*certificates);
    data->key = std::move(key);
    return Identity{std::move(data)};
  }

  /**
   * @brief 同步读取证书链与私钥，并验证链顺序及私钥匹配。
   * @param chain_path 证书链路径。
   * @param key_path 私钥路径。
   * @return 可共享身份，或读取/配置错误。
   */
  static auto from_pem_files(std::filesystem::path chain_path, std::filesystem::path key_path)
      -> expected<Identity> {
    auto chain = detail::read_config_file(chain_path);
    if (!chain)
      return std::unexpected{chain.error()};
    auto key = detail::read_config_file(key_path);
    if (!key)
      return std::unexpected{key.error()};
    return from_pem(std::move(*chain), std::move(*key));
  }

 private:
  /** @brief 允许内部桥接访问私有共享存储，不向公开 API 暴露原生所有权。 */
  friend struct detail::ConfigAccess;

  /** @brief 共享已校验的叶证书优先证书链及匹配私钥。 */
  explicit Identity(std::shared_ptr<const detail::IdentityData> data) : data_(std::move(data)) {}

  std::shared_ptr<const detail::IdentityData> data_; /**< 构建后只读共享的身份材料。 */
};

namespace detail {
/** @brief 构建期间可修改、构建完成后以 const 共享的安全参数。 */
struct ConfigData {
  SharedSslContext context;         /**< 验证完毕后发布的共享 SSL_CTX。 */
  std::optional<TrustStore> roots;  /**< 证书验证所需的显式信任根。 */
  std::optional<Identity> identity; /**< 本端出示的可选证书和私钥。 */
  TlsVersion minimum{TlsVersion::v1_2}, maximum{TlsVersion::v1_3}; /**< 最高允许协商版本。 */
  ClientAuth auth{ClientAuth::none}; /**< 服务端对客户端证书的认证策略。 */
  std::vector<std::string> alpn;     /**< 按优先级排列的 ALPN 名称。 */
  std::vector<unsigned char> wire;   /**< ALPN 的长度前缀编码。 */
};
}  // namespace detail
/** @brief 客户端信任、协议版本与 ALPN 的配置草稿。 */
class ClientConfigBuilder;
/** @brief 服务端配置草稿，包含身份、ALPN 与客户端认证策略。 */
class ServerConfigBuilder;

/** @brief 完成安全验证和 SSL_CTX 初始化的不可变客户端配置。 */
class ClientConfig {
 public:
  /** @brief 禁止默认构造缺少必要材料的客户端配置。 */
  ClientConfig() = delete;
  /** @brief 复制客户端配置的共享所有权。 */
  ClientConfig(const ClientConfig&) noexcept = default;
  /** @brief 复制赋值客户端配置，不复制底层安全材料。 */
  auto operator=(const ClientConfig&) noexcept -> ClientConfig& = default;
  /** @brief 转移客户端配置的所有权，源对象不再持有该资源。 */
  ClientConfig(ClientConfig&&) noexcept = default;
  /** @brief 移动赋值；先放弃目标已有的所有权，再接管源对象。 */
  auto operator=(ClientConfig&&) noexcept -> ClientConfig& = default;
  /** @brief 释放持有的客户端配置。 */
  ~ClientConfig() noexcept = default;
  /**
   * @brief 用必需的信任根或身份创建配置草稿。
   * @param roots 客户端用于验证服务端的信任根。
   * @return 尚可修改的配置构建器。
   */
  static auto builder(TrustStore roots) -> ClientConfigBuilder;
  /**
   * @brief 复制当前安全参数，生成独立草稿，保留原配置。
   * @return 独立的配置构建器。
   */
  auto to_builder() const -> ClientConfigBuilder;

  /**
   * @brief 读取允许协商的最低 TLS 版本。
   * @return 最低协议版本。
   */
  auto min_version() const noexcept -> TlsVersion { return data_->minimum; }

  /**
   * @brief 读取允许协商的最高 TLS 版本。
   * @return 最高协议版本。
   */
  auto max_version() const noexcept -> TlsVersion { return data_->maximum; }

  /**
   * @brief 借用当前配置中的 ALPN 优先级列表。
   * @return 有效期受配置对象所有权约束的只读视图。
   */
  auto alpn_protocols() const noexcept -> std::span<const std::string> { return data_->alpn; }

 private:
  /** @brief 允许客户端构建器发布通过验证的不可变配置。 */
  friend class ClientConfigBuilder;
  /** @brief 允许内部桥接访问私有共享存储，不向公开 API 暴露原生所有权。 */
  friend struct detail::ConfigAccess;

  /** @brief 保存已由 build_context 验证并初始化的客户端配置。 */
  explicit ClientConfig(std::shared_ptr<const detail::ConfigData> data) : data_(std::move(data)) {}

  std::shared_ptr<const detail::ConfigData> data_; /**< 不可变安全配置的共享存储。 */
};

/** @brief 完成安全验证和 SSL_CTX 初始化的不可变服务端配置。 */
class ServerConfig {
 public:
  /** @brief 禁止默认构造缺少必要材料的服务端配置。 */
  ServerConfig() = delete;
  /** @brief 复制服务端配置的共享所有权。 */
  ServerConfig(const ServerConfig&) noexcept = default;
  /** @brief 复制赋值服务端配置，不复制底层安全材料。 */
  auto operator=(const ServerConfig&) noexcept -> ServerConfig& = default;
  /** @brief 转移服务端配置的所有权，源对象不再持有该资源。 */
  ServerConfig(ServerConfig&&) noexcept = default;
  /** @brief 移动赋值；先放弃目标已有的所有权，再接管源对象。 */
  auto operator=(ServerConfig&&) noexcept -> ServerConfig& = default;
  /** @brief 释放持有的服务端配置。 */
  ~ServerConfig() noexcept = default;
  /**
   * @brief 用必需的信任根或身份创建配置草稿。
   * @param identity 服务端出示的证书链与私钥。
   * @return 尚可修改的配置构建器。
   */
  static auto builder(Identity identity) -> ServerConfigBuilder;
  /**
   * @brief 复制当前安全参数，生成独立草稿，保留原配置。
   * @return 独立的配置构建器。
   */
  auto to_builder() const -> ServerConfigBuilder;

  /**
   * @brief 读取允许协商的最低 TLS 版本。
   * @return 最低协议版本。
   */
  auto min_version() const noexcept -> TlsVersion { return data_->minimum; }

  /**
   * @brief 读取允许协商的最高 TLS 版本。
   * @return 最高协议版本。
   */
  auto max_version() const noexcept -> TlsVersion { return data_->maximum; }

  /**
   * @brief 借用当前配置中的 ALPN 优先级列表。
   * @return 有效期受配置对象所有权约束的只读视图。
   */
  auto alpn_protocols() const noexcept -> std::span<const std::string> { return data_->alpn; }

  /**
   * @brief 读取服务端对客户端证书的认证策略。
   * @return 不验证、可选验证或强制验证。
   */
  auto client_auth() const noexcept -> ClientAuth { return data_->auth; }

 private:
  /** @brief 允许服务端构建器发布通过验证的不可变配置。 */
  friend class ServerConfigBuilder;
  /** @brief 允许内部桥接访问私有共享存储，不向公开 API 暴露原生所有权。 */
  friend struct detail::ConfigAccess;

  /** @brief 保存已构建的服务端配置，维持 ALPN 回调参数的共享生命周期。 */
  explicit ServerConfig(std::shared_ptr<const detail::ConfigData> data) : data_(std::move(data)) {}

  std::shared_ptr<const detail::ConfigData> data_; /**< 不可变安全配置的共享存储。 */
};

namespace detail {
/** @brief 供连接实现访问私有配置存储的受控桥接。 */
struct ConfigAccess {
  /**
   * @brief 借用信任根内部的 X509_STORE，不转移所有权。
   * @param roots 待访问的信任根。
   * @return 原生存储指针，移动后对象可能为空。
   */
  static auto store(const TrustStore& roots) noexcept -> X509_STORE* { return roots.store_.get(); }

  /**
   * @brief 借用身份的证书链和私钥存储。
   * @param identity 待访问的身份。
   * @return 共享存储的只读指针。
   */
  static auto identity(const Identity& identity) noexcept -> const IdentityData* {
    return identity.data_.get();
  }

  /**
   * @brief 借用配置的不可变共享存储。
   * @param config 已构建配置。
   * @return 共享存储引用，避免额外复制配置内容。
   */
  static auto data(const ClientConfig& config) noexcept
      -> const std::shared_ptr<const ConfigData>& {
    return config.data_;
  }

  /**
   * @brief 借用配置的不可变共享存储。
   * @param config 已构建配置。
   * @return 共享存储引用，避免额外复制配置内容。
   */
  static auto data(const ServerConfig& config) noexcept
      -> const std::shared_ptr<const ConfigData>& {
    return config.data_;
  }

  /**
   * @brief 从已验证的共享存储构造不可变客户端配置。
   * @return 共享配置值。
   */
  static ClientConfig client(std::shared_ptr<const ConfigData> data) {
    return ClientConfig{std::move(data)};
  }
};

/**
 * @brief 校验安全选项并一次性创建不可变 SSL_CTX。
 * @param data 待冻结的配置草稿。
 * @param server 是否采用服务端身份和客户端认证策略。
 * @return 成功或包含 OpenSSL 诊断的配置错误。
 */
inline auto build_context(ConfigData& data, bool server) -> expected<void> {
  auto valid_version = [](TlsVersion value) {
    return value == TlsVersion::v1_2 || value == TlsVersion::v1_3;
  };
  if (!valid_version(data.minimum) || !valid_version(data.maximum) || data.minimum > data.maximum
      || (data.auth != ClientAuth::none && data.auth != ClientAuth::optional
          && data.auth != ClientAuth::required))
    return std::unexpected{
        tls_error(errc::invalid_config, "Invalid TLS version or client authentication mode")};
  auto wire = alpn_wire(data.alpn);
  if (!wire)
    return std::unexpected{wire.error()};
  data.wire = std::move(*wire);
  ERR_clear_error();
  SslContextOwner context{SSL_CTX_new(TLS_method())};
  if (!context)
    return std::unexpected{tls_error(errc::invalid_config, "Cannot allocate SSL_CTX")};
  SSL_CTX_set_options(context.get(), SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET);
  SSL_CTX_set_session_cache_mode(context.get(), SSL_SESS_CACHE_OFF);
  SSL_CTX_set_max_early_data(context.get(), 0);
  SSL_CTX_set_post_handshake_auth(context.get(), 0);
  SSL_CTX_set_num_tickets(context.get(), 0);
  SSL_CTX_set_max_cert_list(context.get(), 1024 * 1024);
  SSL_CTX_set_mode(context.get(), SSL_MODE_ENABLE_PARTIAL_WRITE);
  if (SSL_CTX_set_min_proto_version(context.get(), static_cast<int>(data.minimum)) != 1
      || SSL_CTX_set_max_proto_version(context.get(), static_cast<int>(data.maximum)) != 1)
    return std::unexpected{tls_error(errc::invalid_config, "Cannot configure TLS versions")};
  if ((!server || data.auth != ClientAuth::none)
      && (!data.roots || !ConfigAccess::store(*data.roots)))
    return std::unexpected{
        tls_error(errc::invalid_config, "TLS verification requires a trust store")};
  if (data.roots) {
    if (!ConfigAccess::store(*data.roots))
      return std::unexpected{tls_error(errc::invalid_config, "Moved-from TLS trust store")};
    SSL_CTX_set1_cert_store(context.get(), ConfigAccess::store(*data.roots));
  }
  const int verify =
      server
          ? (data.auth == ClientAuth::none
                 ? SSL_VERIFY_NONE
                 : SSL_VERIFY_PEER
                       | (data.auth == ClientAuth::required ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0))
          : SSL_VERIFY_PEER;
  SSL_CTX_set_verify(context.get(), verify, nullptr);
  if (server && !data.identity)
    return std::unexpected{tls_error(errc::invalid_config, "Server identity is required")};
  if (data.identity) {
    auto* identity = ConfigAccess::identity(*data.identity);
    if (!identity || identity->certificates.empty() || !identity->key
        || SSL_CTX_use_certificate(context.get(), identity->certificates.front().get()) != 1
        || SSL_CTX_use_PrivateKey(context.get(), identity->key.get()) != 1
        || SSL_CTX_check_private_key(context.get()) != 1)
      return std::unexpected{tls_error(errc::invalid_config, "Cannot install TLS identity")};
    for (std::size_t i = 1; i < identity->certificates.size(); ++i)
      if (SSL_CTX_add1_chain_cert(context.get(), identity->certificates[i].get()) != 1)
        return std::unexpected{
            tls_error(errc::invalid_config, "Cannot install TLS certificate chain")};
  }
  data.context = SharedSslContext(context.release(), SSL_CTX_free);
  return {};
}
}  // namespace detail

/** @brief 客户端配置草稿，build 不修改草稿本身。 */
class ClientConfigBuilder {
 public:
  /** @brief 禁止默认构造缺少必要材料的客户端配置草稿。 */
  ClientConfigBuilder() = delete;
  /** @brief 复制客户端配置草稿，保留底层安全材料的共享所有权。 */
  ClientConfigBuilder(const ClientConfigBuilder&) = default;
  /** @brief 复制赋值客户端配置草稿，共享底层安全材料。 */
  auto operator=(const ClientConfigBuilder&) -> ClientConfigBuilder& = default;
  /** @brief 移动构造客户端配置草稿。 */
  ClientConfigBuilder(ClientConfigBuilder&&) noexcept = default;
  /** @brief 移动赋值；先放弃目标已有的所有权，再接管源对象。 */
  auto operator=(ClientConfigBuilder&&) noexcept -> ClientConfigBuilder& = default;
  /** @brief 释放持有的客户端配置草稿。 */
  ~ClientConfigBuilder() noexcept = default;

  /**
   * @brief 为客户端配置双向认证所需的身份。
   * @param identity 将要出示的客户端证书和私钥。
   * @return 当前构建器引用。
   */
  auto with_identity(Identity identity) -> ClientConfigBuilder& {
    data_.identity = std::move(identity);
    return *this;
  }

  /**
   * @brief 在配置草稿中设置 ALPN 列表，合法性由 build 统一验证。
   * @param protocols 按本端优先级排列的协议名称。
   * @return 当前构建器引用，以便继续配置其他参数。
   */
  auto with_alpn(std::vector<std::string> protocols) -> ClientConfigBuilder& {
    data_.alpn = std::move(protocols);
    return *this;
  }

  /**
   * @brief 设置协商的协议版本范围，实际合法性由 build 验证。
   * @param min 最低版本。
   * @param max 最高版本。
   * @return 当前构建器引用。
   */
  auto with_versions(TlsVersion min, TlsVersion max) -> ClientConfigBuilder& {
    data_.minimum = min;
    data_.maximum = max;
    return *this;
  }

  /**
   * @brief 验证全部草稿参数并生成可共享的不可变安全配置。
   * @return 已构建配置，或配置错误；不改变当前草稿。
   */
  auto build() const -> expected<ClientConfig> {
    auto data = std::make_shared<detail::ConfigData>(data_);
    auto result = detail::build_context(*data, false);
    if (!result)
      return std::unexpected{result.error()};
    return ClientConfig{std::move(data)};
  }

 private:
  /** @brief 允许客户端配置的 builder/to_builder 入口构造草稿。 */
  friend class ClientConfig;

  /**
   * @brief 接收客户端配置草稿；后续修改不会改变已经发布的 ClientConfig。
   * @param data 以值传入、可继续编辑的配置草稿。
   */
  explicit ClientConfigBuilder(detail::ConfigData data) : data_(std::move(data)) {}

  detail::ConfigData data_; /**< 尚可修改的配置草稿。 */
};

/** @brief 服务端配置草稿，包含身份、ALPN 与客户端认证策略。 */
class ServerConfigBuilder {
 public:
  /** @brief 禁止默认构造缺少必要材料的服务端配置草稿。 */
  ServerConfigBuilder() = delete;
  /** @brief 复制服务端配置草稿，保留底层安全材料的共享所有权。 */
  ServerConfigBuilder(const ServerConfigBuilder&) = default;
  /** @brief 复制赋值服务端配置草稿，共享底层安全材料。 */
  auto operator=(const ServerConfigBuilder&) -> ServerConfigBuilder& = default;
  /** @brief 移动构造服务端配置草稿。 */
  ServerConfigBuilder(ServerConfigBuilder&&) noexcept = default;
  /** @brief 移动赋值；先放弃目标已有的所有权，再接管源对象。 */
  auto operator=(ServerConfigBuilder&&) noexcept -> ServerConfigBuilder& = default;
  /** @brief 释放持有的服务端配置草稿。 */
  ~ServerConfigBuilder() noexcept = default;

  /**
   * @brief 在配置草稿中设置 ALPN 列表，合法性由 build 统一验证。
   * @param protocols 按本端优先级排列的协议名称。
   * @return 当前构建器引用，以便继续配置其他参数。
   */
  auto with_alpn(std::vector<std::string> protocols) -> ServerConfigBuilder& {
    data_.alpn = std::move(protocols);
    return *this;
  }

  /**
   * @brief 设置协商的协议版本范围，实际合法性由 build 验证。
   * @param min 最低版本。
   * @param max 最高版本。
   * @return 当前构建器引用。
   */
  auto with_versions(TlsVersion min, TlsVersion max) -> ServerConfigBuilder& {
    data_.minimum = min;
    data_.maximum = max;
    return *this;
  }

  /**
   * @brief 设置服务端对客户端证书的验证要求。
   * @param mode 可选或强制客户端认证策略。
   * @param roots 验证客户端证书使用的信任根。
   * @return 当前构建器引用。
   */
  auto with_client_auth(ClientAuth mode, TrustStore roots) -> ServerConfigBuilder& {
    data_.auth = mode;
    data_.roots = std::move(roots);
    return *this;
  }

  /**
   * @brief 关闭客户端证书验证并移除对应信任根。
   * @return 当前构建器引用。
   */
  auto without_client_auth() -> ServerConfigBuilder& {
    data_.auth = ClientAuth::none;
    data_.roots.reset();
    return *this;
  }

  /**
   * @brief 验证全部草稿参数并生成可共享的不可变安全配置。
   * @return 已构建配置，或配置错误；不改变当前草稿。
   */
  auto build() const -> expected<ServerConfig> {
    auto data = std::make_shared<detail::ConfigData>(data_);
    auto result = detail::build_context(*data, true);
    if (!result)
      return std::unexpected{result.error()};

    SSL_CTX_set_alpn_select_cb(
        data->context.get(),
        [](SSL*,
           const unsigned char** output,
           unsigned char* output_size,
           const unsigned char* input,
           unsigned int input_size,
           void* arg) -> int {
          const auto& config = *static_cast<const detail::ConfigData*>(arg);
          if (config.alpn.empty() || input_size == 0)
            return SSL_TLSEXT_ERR_NOACK;
          for (const auto& wanted : config.alpn) {
            unsigned int position{};
            while (position < input_size) {
              const auto size = input[position++];
              if (!size || size > input_size - position)
                return SSL_TLSEXT_ERR_ALERT_FATAL;
              if (wanted.size() == size
                  && std::memcmp(wanted.data(), input + position, size) == 0) {
                *output = input + position;
                *output_size = size;
                return SSL_TLSEXT_ERR_OK;
              }
              position += size;
            }
          }
          return SSL_TLSEXT_ERR_ALERT_FATAL;
        },
        data.get());
    return ServerConfig{std::move(data)};
  }

 private:
  /** @brief 允许服务端配置的 builder/to_builder 入口构造草稿。 */
  friend class ServerConfig;

  /**
   * @brief 接收服务端配置草稿；后续修改不会改变已经发布的 ServerConfig。
   * @param data 以值传入、可继续编辑的配置草稿。
   */
  explicit ServerConfigBuilder(detail::ConfigData data) : data_(std::move(data)) {}

  detail::ConfigData data_; /**< 尚可修改的配置草稿。 */
};

/**
 * @brief 用必需的信任根或身份创建配置草稿。
 * @param roots 客户端用于验证服务端的信任根。
 * @return 尚可修改的配置构建器。
 */
inline auto ClientConfig::builder(TrustStore roots) -> ClientConfigBuilder {
  detail::ConfigData data;
  data.roots = std::move(roots);
  return ClientConfigBuilder{std::move(data)};
}

/**
 * @brief 复制当前安全参数，生成独立草稿，保留原配置。
 * @return 独立的配置构建器。
 */
inline auto ClientConfig::to_builder() const -> ClientConfigBuilder {
  return ClientConfigBuilder{data_ ? *data_ : detail::ConfigData{}};
}

/**
 * @brief 用必需的信任根或身份创建配置草稿。
 * @param identity 服务端出示的证书链与私钥。
 * @return 尚可修改的配置构建器。
 */
inline auto ServerConfig::builder(Identity identity) -> ServerConfigBuilder {
  detail::ConfigData data;
  data.identity = std::move(identity);
  return ServerConfigBuilder{std::move(data)};
}

/**
 * @brief 复制当前安全参数，生成独立草稿，保留原配置。
 * @return 独立的配置构建器。
 */
inline auto ServerConfig::to_builder() const -> ServerConfigBuilder {
  return ServerConfigBuilder{data_ ? *data_ : detail::ConfigData{}};
}
}  // namespace faio::tls
