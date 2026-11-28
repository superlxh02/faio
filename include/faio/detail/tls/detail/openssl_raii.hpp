/**
 * @file faio/detail/tls/detail/openssl_raii.hpp
 * @brief 使用唯一或共享所有权管理 OpenSSL 原生对象。
 */
#pragma once
#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <memory>

namespace faio::tls::detail {
/** @brief 调用匹配释放函数的 OpenSSL 对象删除器。 */
template <class T, auto Free>
struct open_ssl_deleter {
  /**
   * @brief 若指针非空，调用与其 OpenSSL 类型匹配的释放函数。
   * @param pointer 当前智能指针独占拥有的原生对象。
   */
  void operator()(T* pointer) const noexcept {
    if (pointer)
      static_cast<void>(Free(pointer));
  }
};

/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using ssl_context_owner = std::unique_ptr<SSL_CTX, open_ssl_deleter<SSL_CTX, SSL_CTX_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using ssl_owner = std::unique_ptr<SSL, open_ssl_deleter<SSL, SSL_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using bio_owner = std::unique_ptr<BIO, open_ssl_deleter<BIO, BIO_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using certificate_owner = std::unique_ptr<X509, open_ssl_deleter<X509, X509_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using certificate_store_owner =
    std::unique_ptr<X509_STORE, open_ssl_deleter<X509_STORE, X509_STORE_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using private_key_owner = std::unique_ptr<EVP_PKEY, open_ssl_deleter<EVP_PKEY, EVP_PKEY_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using digest_owner = std::unique_ptr<EVP_MD, open_ssl_deleter<EVP_MD, EVP_MD_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using digest_context_owner =
    std::unique_ptr<EVP_MD_CTX, open_ssl_deleter<EVP_MD_CTX, EVP_MD_CTX_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using shared_ssl_context = std::shared_ptr<SSL_CTX>;
}  // namespace faio::tls::detail
