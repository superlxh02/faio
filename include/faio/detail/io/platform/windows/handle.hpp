/**
 * @file faio/detail/io/platform/windows/handle.hpp
 * @brief Windows 句柄 RAII。
 */
#pragma once
#include <cstdint>
#include <utility>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#endif
namespace faio::io::windows {
#if defined(_WIN32)
using socket_handle = SOCKET;
using file_handle = HANDLE;
inline constexpr socket_handle invalid_socket_handle = INVALID_SOCKET;

/** @brief 取得平台无效文件句柄值。 */
inline file_handle invalid_file_handle() noexcept {
  return INVALID_HANDLE_VALUE;
}
#else
/** @brief 平台无关的原生句柄表示。 */
using socket_handle = std::uintptr_t;
using file_handle = void*;
inline constexpr socket_handle invalid_socket_handle = ~socket_handle{};

/** @brief 取得平台无效文件句柄值。 */
inline file_handle invalid_file_handle() noexcept {
  return reinterpret_cast<file_handle>(~std::uintptr_t{});
}
#endif
/**  @brief 独占原生 socket；只支持显式 release 和恰好一次 closesocket。 */
class owned_socket_handle {
 public:
  /** @brief 构造独占原生套接字句柄。 */
  owned_socket_handle() noexcept = default;

  /** @brief 构造独占原生套接字句柄。 */
  explicit owned_socket_handle(socket_handle handle) noexcept : handle_(handle) {}

  /** @brief 禁止复制构造独占原生套接字句柄。 */
  owned_socket_handle(const owned_socket_handle&) = delete;

  /** @brief 禁止复制赋值。 */
  owned_socket_handle& operator=(const owned_socket_handle&) = delete;

  /** @brief 移动构造独占原生套接字句柄。 */
  owned_socket_handle(owned_socket_handle&& other) noexcept : handle_(other.release()) {}

  /** @brief 移动赋值。 */
  owned_socket_handle& operator=(owned_socket_handle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.release();
    }
    return *this;
  }

  /** @brief 销毁独占原生套接字句柄。 */
  ~owned_socket_handle() { reset(); }

  /** @brief 借用原生句柄，不转交关闭责任。 */
  socket_handle get() const noexcept { return handle_; }

  /** @brief 转出原生句柄及关闭责任，并置空源对象。 */
  socket_handle release() noexcept { return std::exchange(handle_, invalid_socket_handle); }

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return handle_ != invalid_socket_handle; }

  /** @brief 关闭当前句柄并接管替换句柄。 */
  void reset(socket_handle replacement = invalid_socket_handle) noexcept {
    const auto previous = std::exchange(handle_, replacement);
#if defined(_WIN32)
    if (previous != invalid_socket_handle)
      (void)::closesocket(previous);
#else
    (void)previous;
#endif
  }

 private:
  socket_handle handle_{
      invalid_socket_handle}; /**< 独占拥有的原生句柄，移动或释放后置为平台无效值。 */
};

/**  @brief 独占文件 HANDLE；CloseHandle 与 closesocket 的所有权绝不互换。 */
class owned_file_handle {
 public:
  /** @brief 构造独占原生文件句柄。 */
  owned_file_handle() noexcept = default;

  /** @brief 构造独占原生文件句柄。 */
  explicit owned_file_handle(file_handle handle) noexcept : handle_(handle) {}

  /** @brief 禁止复制构造独占原生文件句柄。 */
  owned_file_handle(const owned_file_handle&) = delete;

  /** @brief 禁止复制赋值。 */
  owned_file_handle& operator=(const owned_file_handle&) = delete;

  /** @brief 移动构造独占原生文件句柄。 */
  owned_file_handle(owned_file_handle&& other) noexcept : handle_(other.release()) {}

  /** @brief 移动赋值。 */
  owned_file_handle& operator=(owned_file_handle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.release();
    }
    return *this;
  }

  /** @brief 销毁独占原生文件句柄。 */
  ~owned_file_handle() { reset(); }

  /** @brief 借用原生句柄，不转交关闭责任。 */
  file_handle get() const noexcept { return handle_; }

  /** @brief 转出原生句柄及关闭责任，并置空源对象。 */
  file_handle release() noexcept { return std::exchange(handle_, invalid_file_handle()); }

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return handle_ && handle_ != invalid_file_handle(); }

  /** @brief 关闭当前句柄并接管替换句柄。 */
  void reset(file_handle replacement = invalid_file_handle()) noexcept {
    const auto previous = std::exchange(handle_, replacement);
#if defined(_WIN32)
    if (previous && previous != invalid_file_handle())
      (void)::CloseHandle(previous);
#else
    (void)previous;
#endif
  }

 private:
  file_handle handle_{
      invalid_file_handle()}; /**< 独占拥有的原生句柄，移动或释放后置为平台无效值。 */
};
}  // namespace faio::io::windows
