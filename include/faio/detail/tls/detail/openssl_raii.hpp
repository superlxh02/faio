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
struct OpenSslDeleter {
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
using SslContextOwner = std::unique_ptr<SSL_CTX, OpenSslDeleter<SSL_CTX, SSL_CTX_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using SslOwner = std::unique_ptr<SSL, OpenSslDeleter<SSL, SSL_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using BioOwner = std::unique_ptr<BIO, OpenSslDeleter<BIO, BIO_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using CertificateOwner = std::unique_ptr<X509, OpenSslDeleter<X509, X509_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using CertificateStoreOwner =
    std::unique_ptr<X509_STORE, OpenSslDeleter<X509_STORE, X509_STORE_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using PrivateKeyOwner = std::unique_ptr<EVP_PKEY, OpenSslDeleter<EVP_PKEY, EVP_PKEY_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using DigestOwner = std::unique_ptr<EVP_MD, OpenSslDeleter<EVP_MD, EVP_MD_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using DigestContextOwner = std::unique_ptr<EVP_MD_CTX, OpenSslDeleter<EVP_MD_CTX, EVP_MD_CTX_free>>;
/** @brief 采用智能指针管理的 OpenSSL 对象所有权类型。 */
using SharedSslContext = std::shared_ptr<SSL_CTX>;
}  // namespace faio::tls::detail
