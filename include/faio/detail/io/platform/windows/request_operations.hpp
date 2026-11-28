/**
 * @file faio/detail/io/platform/windows/request_operations.hpp
 * @brief 统一请求系统调用实现。
 */
#pragma once
#include "faio/detail/io/core/request_rules.hpp"
#include "faio/detail/io/core/resource_state.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <limits>
#include "faio/detail/io/platform/windows/file.hpp"
#include "faio/detail/io/platform/windows/socket.hpp"

namespace faio::io::detail::windows_operations {
/** @brief 根据请求种类及资源属性判定是否需要文件执行路径。 */
inline bool is_file_request(const io_request& req) noexcept {
  return req.kind == operation_kind::OPEN || req.kind == operation_kind::OPEN2
         || req.kind == operation_kind::FSYNC
         || ((req.kind == operation_kind::READ || req.kind == operation_kind::WRITE
              || req.kind == operation_kind::READV || req.kind == operation_kind::WRITEV)
             && req.native_kind == native_handle_kind::WINDOWS_HANDLE)
         || (req.kind == operation_kind::CLOSE
             && req.native_kind == native_handle_kind::WINDOWS_HANDLE);
}

/** @brief 检查原生关闭是否可能阻塞，决定是否交给隔离清理服务。 */
inline bool needs_deferred_close(const resource_state& resource, native_descriptor fd) noexcept {
  if (resource.native_kind == native_handle_kind::WINDOWS_HANDLE)
    return true;
  if (resource.native_kind == native_handle_kind::WINDOWS_PIPE)
    return false;
  ::linger option{};
  int length = sizeof(option);
  return ::getsockopt(static_cast<SOCKET>(fd),
                      SOL_SOCKET,
                      SO_LINGER,
                      reinterpret_cast<char*>(&option),
                      &length)
             == 0
         && option.l_onoff && option.l_linger > 0;
}

/** @brief 执行原生句柄关闭。 */
inline int close_native(native_descriptor fd, native_handle_kind kind) noexcept {
  if (kind == native_handle_kind::WINDOWS_HANDLE || kind == native_handle_kind::WINDOWS_PIPE)
    return ::CloseHandle(reinterpret_cast<HANDLE>(fd))
               ? 0
               : windows::encode_windows_error(::GetLastError());
  return ::closesocket(static_cast<SOCKET>(fd)) == 0
             ? 0
             : windows::encode_winsock_error(::WSAGetLastError());
}

/** @brief 执行统一 RECV 或 SEND 非阻塞系统调用并立即捕获错误。 */
template <class Request>
inline std::int64_t perform_socket_scalar(Request& r) noexcept {
  const int size = static_cast<int>(std::min<std::size_t>(r.length, INT_MAX));
  const int value =
      r.kind == operation_kind::RECV
          ? ::recv(static_cast<SOCKET>(r.fd), static_cast<char*>(r.buffer), size, r.flags)
          : ::send(
                static_cast<SOCKET>(r.fd), static_cast<const char*>(r.const_buffer), size, r.flags);
  return value == SOCKET_ERROR ? -windows::encode_winsock_error(::WSAGetLastError()) : value;
}

/** @brief 按统一请求种类执行真实系统调用，保留短读写进度与原生错误。 */
inline std::int64_t perform(io_request& r, bool&) noexcept {
  const SOCKET socket = static_cast<SOCKET>(r.fd);
  int result{};
  switch (r.kind) {
    case operation_kind::SHUTDOWN:
      result = ::shutdown(socket, r.argument);
      break;
    case operation_kind::SOCKET: {
      if (r.flags)
        return -EOPNOTSUPP;
      const SOCKET created = ::WSASocketW(r.argument,
                                          r.argument2 & ~(SOCK_NONBLOCK | SOCK_CLOEXEC),
                                          r.argument3,
                                          nullptr,
                                          0,
                                          WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
      if (created == INVALID_SOCKET)
        return -windows::encode_winsock_error(::WSAGetLastError());
      u_long mode = 1;
      if (::ioctlsocket(created, FIONBIO, &mode) == SOCKET_ERROR) {
        auto error = ::WSAGetLastError();
        ::closesocket(created);
        return -windows::encode_winsock_error(error);
      }
      return static_cast<std::int64_t>(created);
    }
    case operation_kind::GETSOCKOPT: {
      int length = static_cast<int>(r.length);
      result = ::getsockopt(socket, r.argument, r.argument2, static_cast<char*>(r.buffer), &length);
      if (!result)
        return length;
      break;
    }
    case operation_kind::SETSOCKOPT:
      result = ::setsockopt(socket,
                            r.argument,
                            r.argument2,
                            static_cast<const char*>(r.const_buffer),
                            static_cast<int>(r.length));
      break;
    case operation_kind::SOCKET_INQ: {
      u_long bytes{};
      result = ::ioctlsocket(socket, FIONREAD, &bytes);
      if (!result)
        return bytes;
      break;
    }
    case operation_kind::SOCKET_OUTQ:
      return -EOPNOTSUPP;
    case operation_kind::ACCEPT_NOWAIT: {
      SOCKET accepted = ::accept(socket, r.output_address, r.output_address_length);
      if (accepted == INVALID_SOCKET) {
        auto e = ::WSAGetLastError();
        return e == WSAEWOULDBLOCK ? -EAGAIN : -windows::encode_winsock_error(e);
      }
      return static_cast<std::int64_t>(accepted);
    }
    case operation_kind::OPEN:
      return windows::open_file(r);
    case operation_kind::READ:
    case operation_kind::WRITE:
      return windows::scalar_file(r);
    case operation_kind::READV:
    case operation_kind::WRITEV:
      return windows::vectored_file(r);
    case operation_kind::CLOSE:
      return -close_native(r.fd, r.native_kind);
    case operation_kind::FSYNC:
      return ::FlushFileBuffers(reinterpret_cast<HANDLE>(r.fd))
                 ? 0
                 : -windows::encode_windows_error(::GetLastError());
    default:
      return -EOPNOTSUPP;
  }
  return result == SOCKET_ERROR ? -windows::encode_winsock_error(::WSAGetLastError()) : result;
}
}  // namespace faio::io::detail::windows_operations
