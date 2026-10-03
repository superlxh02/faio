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
/** @brief SOCKET 与 HANDLE 分开建模，禁止用 POSIX int 或同一个关闭器混用。 */
#if defined(_WIN32)
using socket_handle = SOCKET;
using file_handle = HANDLE;
inline constexpr socket_handle invalid_socket_handle = INVALID_SOCKET;

inline file_handle invalid_file_handle() noexcept {
  return INVALID_HANDLE_VALUE;
}
#else
// 非 Windows 上也能编译协议和布局 smoke test，完全不引入 POSIX 系统头。
using socket_handle = std::uintptr_t;
using file_handle = void*;
inline constexpr socket_handle invalid_socket_handle = ~socket_handle{};

inline file_handle invalid_file_handle() noexcept {
  return reinterpret_cast<file_handle>(~std::uintptr_t{});
}
#endif
/** @brief 独占原生 socket；只支持显式 release 和恰好一次 closesocket。 */
class owned_socket_handle {
 public:
  owned_socket_handle() noexcept = default;

  explicit owned_socket_handle(socket_handle handle) noexcept : handle_(handle) {}

  owned_socket_handle(const owned_socket_handle&) = delete;

  owned_socket_handle& operator=(const owned_socket_handle&) = delete;

  owned_socket_handle(owned_socket_handle&& other) noexcept : handle_(other.release()) {}

  owned_socket_handle& operator=(owned_socket_handle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.release();
    }
    return *this;
  }

  ~owned_socket_handle() { reset(); }

  socket_handle get() const noexcept { return handle_; }

  socket_handle release() noexcept { return std::exchange(handle_, invalid_socket_handle); }

  explicit operator bool() const noexcept { return handle_ != invalid_socket_handle; }

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
  socket_handle handle_{invalid_socket_handle};
};

/** @brief 独占文件 HANDLE；CloseHandle 与 closesocket 的所有权绝不互换。 */
class owned_file_handle {
 public:
  owned_file_handle() noexcept = default;

  explicit owned_file_handle(file_handle handle) noexcept : handle_(handle) {}

  owned_file_handle(const owned_file_handle&) = delete;

  owned_file_handle& operator=(const owned_file_handle&) = delete;

  owned_file_handle(owned_file_handle&& other) noexcept : handle_(other.release()) {}

  owned_file_handle& operator=(owned_file_handle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.release();
    }
    return *this;
  }

  ~owned_file_handle() { reset(); }

  file_handle get() const noexcept { return handle_; }

  file_handle release() noexcept { return std::exchange(handle_, invalid_file_handle()); }

  explicit operator bool() const noexcept { return handle_ && handle_ != invalid_file_handle(); }

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
  file_handle handle_{invalid_file_handle()};
};
}  // namespace faio::io::windows
