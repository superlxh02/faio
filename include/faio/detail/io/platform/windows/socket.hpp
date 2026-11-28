/**
 * @file faio/detail/io/platform/windows/socket.hpp
 * @brief 套接字资源封装。
 */
#pragma once
#include "faio/detail/io/platform/native_types.hpp"
#include "faio/detail/common/error.hpp"
#if defined(_WIN32)
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <limits>
#include <vector>

namespace faio::io::windows {
/**
 *  @brief 将 Winsock 错误映射到引擎内部的 would-block/参数错误分类。
 * @details 公开的 Error 仍保留原生 Winsock 编码；本函数仅用于判断控制流。
 */
inline auto winsock_errno(int error = ::WSAGetLastError()) noexcept -> int {
  switch (error) {
    case 0:
      return 0;
    case WSAEWOULDBLOCK:
      return EAGAIN;
    case WSAEINTR:
      return EINTR;
    case WSAEBADF:
    case WSAENOTSOCK:
      return EBADF;
    case WSAEFAULT:
      return EFAULT;
    case WSAEINVAL:
      return EINVAL;
    case WSAEACCES:
      return EACCES;
    case WSAEMFILE:
      return EMFILE;
    case WSAENOBUFS:
      return ENOMEM;
    case WSAEADDRINUSE:
      return EADDRINUSE;
    case WSAEADDRNOTAVAIL:
      return EADDRNOTAVAIL;
    case WSAEAFNOSUPPORT:
      return EAFNOSUPPORT;
    case WSAEPROTONOSUPPORT:
      return EPROTONOSUPPORT;
    case WSAEOPNOTSUPP:
      return ENOTSUP;
    case WSAEMSGSIZE:
      return EMSGSIZE;
    case WSAECONNREFUSED:
      return ECONNREFUSED;
    case WSAECONNRESET:
      return ECONNRESET;
    case WSAECONNABORTED:
      return ECONNABORTED;
    case WSAENETUNREACH:
      return ENETUNREACH;
    case WSAEHOSTUNREACH:
      return EHOSTUNREACH;
    case WSAENOTCONN:
      return ENOTCONN;
    case WSAESHUTDOWN:
      return EPIPE;
    case WSAETIMEDOUT:
      return ETIMEDOUT;
    case WSAEINPROGRESS:
      return EINPROGRESS;
    case WSAEALREADY:
      return EALREADY;
    case WSA_OPERATION_ABORTED:
      return ECANCELED;
    default:
      return EIO;
  }
}

/**
 *  @brief 同时保存内部 errno 与原生 Winsock 状态，后续不得覆盖 native error。
 * @param error 已取得的原生 Winsock 错误，默认取当前线程的 LastError。
 * @return SOCKET_ERROR；调用方继续使用熟悉的负值失败约定。
 */
inline auto socket_failure(int error = ::WSAGetLastError()) noexcept -> int {
  errno = winsock_errno(error);
  ::WSASetLastError(error);
  return SOCKET_ERROR;
}

/**  @brief 进程级 Winsock 2.2 lease；静态初始化线程安全且初始化失败明确可见。 */
inline auto initialize_winsock() noexcept -> expected<void> {
  struct lease {
    int error{}; /**< 保存操作错误，供操作完成或重复查询时返回。 */

    /** @brief 取得 Winsock 2.2 初始化租约。 */
    lease() noexcept {
      WSADATA data{};
      error = ::WSAStartup(MAKEWORD(2, 2), &data);
      if (!error && data.wVersion != MAKEWORD(2, 2)) {
        ::WSACleanup();
        error = WSAVERNOTSUPPORTED;
      }
    }

    /** @brief 释放 Winsock 初始化租约。 */
    ~lease() {
      if (!error)
        ::WSACleanup();
    }
  };

  static lease network;
  if (network.error)
    return std::unexpected{::faio::error{network.error, 0, error_domain::WINSOCK}};
  return {};
}

/**  @brief 显式转换保留所有 handle 位，INVALID_SOCKET 对应 native_descriptor(-1)。 */
inline auto as_socket(detail::native_descriptor descriptor) noexcept -> SOCKET {
  return static_cast<SOCKET>(descriptor);
}

/**
 *  @brief socket 始终保持非阻塞与禁止继承，显式 try_* 不会卡住协程 worker。
 * @param descriptor 已由唯一拥有者管理的原生 SOCKET；本函数不取得关闭责任。
 * @return 原生错误域中的失败，或配置成功。
 */
inline auto prepare_socket(detail::native_descriptor descriptor) noexcept -> expected<void> {
  u_long enabled = 1;
  if (::ioctlsocket(as_socket(descriptor), FIONBIO, &enabled) == SOCKET_ERROR)
    return std::unexpected{::faio::error{::WSAGetLastError(), 0, error_domain::WINSOCK}};
  if (!::SetHandleInformation(
          reinterpret_cast<HANDLE>(as_socket(descriptor)), HANDLE_FLAG_INHERIT, 0))
    return std::unexpected{
        ::faio::error{static_cast<int>(::GetLastError()), 0, error_domain::WIN32_API}};
  return {};
}

/**  @brief 读取属于此 socket provider 的扩展函数，不能跨 provider 复用裸指针。 */
template <class Function>
inline auto socket_extension(SOCKET socket, GUID identifier, Function& function) noexcept -> bool {
  DWORD bytes{};
  if (::WSAIoctl(socket,
                 SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &identifier,
                 sizeof(identifier),
                 &function,
                 sizeof(function),
                 &bytes,
                 nullptr,
                 nullptr)
      == SOCKET_ERROR) {
    socket_failure();
    return false;
  }
  return function != nullptr;
}

/**
 *  @brief 短非阻塞分散 IO 的 WSABUF 适配；常见少量向量使用栈存储。
 * @details 只复制描述符，绝不复制 payload；过长元素返回 EINVAL，禁止 ULONG 截断。
 */
class socket_vectors {
 public:
  /**  @brief 检查公开 iovec 约定，并建立仅借用 payload 的 WSABUF 数组。 */
  bool prepare(const iovec* vectors, std::size_t count) noexcept {
    if (count > IOV_MAX || (count && !vectors)) {
      socket_failure(count > IOV_MAX ? WSAEINVAL : WSAEFAULT);
      return false;
    }
    try {
      if (count > local_.size()) {
        dynamic_.resize(count);
        data_ = dynamic_.data();
      } else {
        data_ = local_.data();
      }
      for (std::size_t index = 0; index < count; ++index) {
        if (vectors[index].iov_len > std::numeric_limits<ULONG>::max()
            || (vectors[index].iov_len && !vectors[index].iov_base)) {
          socket_failure(vectors[index].iov_len > std::numeric_limits<ULONG>::max() ? WSAEINVAL
                                                                                    : WSAEFAULT);
          return false;
        }
        data_[index] = {static_cast<ULONG>(vectors[index].iov_len),
                        static_cast<char*>(vectors[index].iov_base)};
      }
      count_ = static_cast<DWORD>(count);
      if (!count) {
        local_[0] = {0, &empty_};
        data_ = local_.data();
        count_ = 1;
      }
      return true;
    } catch (...) {
      socket_failure(WSAENOBUFS);
      return false;
    }
  }

  /** @brief 借用当前 WSABUF 描述符数组，描述符与负载的有效期保持不变。 */
  WSABUF* data() noexcept { return data_; }

  /** @brief 返回有效 WSABUF 描述符的数量。 */
  DWORD count() const noexcept { return count_; }

 private:
  std::array<WSABUF, 16> local_{}; /**< 少量缓冲区向量使用的内联数组，避免普通请求堆分配。 */
  std::vector<WSABUF> dynamic_;    /**< 超过栈内向量容量时使用的自有 WSABUF 存储。 */
  WSABUF* data_{};                 /**< 指向当前有效 WSABUF 数组，可能借用栈内数组或动态存储。 */
  DWORD count_{};                  /**< 提交给 Winsock 的有效缓冲区向量个数。 */
  char empty_{};                   /**< 零长度 Winsock 请求使用的有效占位字节。 */
};

/**
 *  @brief 短非阻塞 recvmsg；数据报截断保留已经复制的数据与 MSG_TRUNC 信息。
 * @param descriptor 已配置 FIONBIO 的 SOCKET，不得传入普通文件 HANDLE。
 * @param message 输入容量与输出消息元数据；缓冲区保持在调用方拥有范围内。
 * @param flags MSG_DONTWAIT 为库标志，送入原生 API 前剥离；MSG_PEEK 保留消息。
 * @return 实际复制字节，或 -1；截断成功返回复制量并置 MSG_TRUNC/MSG_CTRUNC。
 * @details 不传 OVERLAPPED，返回后没有内核对栈 WSABUF 的借用责任。
 */
inline auto recv_message(detail::native_descriptor descriptor, msghdr* message, int flags) noexcept
    -> std::intptr_t {
  if (!message)
    return socket_failure(WSAEFAULT);
  socket_vectors vectors;
  if (!vectors.prepare(message->msg_iov, message->msg_iovlen))
    return -1;
  if (message->msg_controllen > std::numeric_limits<ULONG>::max())
    return socket_failure(WSAEINVAL);
  DWORD bytes{}, native_flags = static_cast<DWORD>(flags & ~MSG_DONTWAIT);
  const auto socket = as_socket(descriptor);
  int result{};
  if (message->msg_name || message->msg_control) {
    LPFN_WSARECVMSG receive{};
    if (!socket_extension(socket, WSAID_WSARECVMSG, receive))
      return -1;
    WSAMSG native{
        static_cast<sockaddr*>(message->msg_name),
        message->msg_namelen,
        vectors.data(),
        vectors.count(),
        {static_cast<ULONG>(message->msg_controllen), static_cast<char*>(message->msg_control)},
        native_flags};
    result = receive(socket, &native, &bytes, nullptr, nullptr);
    const auto error = result == SOCKET_ERROR ? ::WSAGetLastError() : 0;
    message->msg_namelen = native.namelen;
    message->msg_controllen = native.Control.len;
    message->msg_flags = static_cast<int>(native.dwFlags);
    if (error && error != WSAEMSGSIZE)
      return socket_failure(error);
    if (error == WSAEMSGSIZE && !(native.dwFlags & (MSG_TRUNC | MSG_CTRUNC)))
      message->msg_flags |= MSG_TRUNC;
  } else {
    result =
        ::WSARecv(socket, vectors.data(), vectors.count(), &bytes, &native_flags, nullptr, nullptr);
    const auto error = result == SOCKET_ERROR ? ::WSAGetLastError() : 0;
    if (error && error != WSAEMSGSIZE)
      return socket_failure(error);
    message->msg_flags = static_cast<int>(native_flags);
    if (error == WSAEMSGSIZE)
      message->msg_flags |= MSG_TRUNC;
    message->msg_controllen = 0;
  }
  return static_cast<std::intptr_t>(bytes);
}

/**
 *  @brief 短非阻塞 sendmsg；控制数据交给 WSASendMsg，普通消息直接 WSASend(To)。
 * @param descriptor 已配置 FIONBIO 的 SOCKET。
 * @param message 借用 payload/控制数据；整个调用结束以前必须保持有效。
 * @param flags 原生发送标志及库私有 MSG_DONTWAIT。
 * @return 已交给传输层的字节，或 -1；UDP 超大消息不会拆成多个包。
 */
inline auto send_message(detail::native_descriptor descriptor,
                         const msghdr* message,
                         int flags) noexcept -> std::intptr_t {
  if (!message)
    return socket_failure(WSAEFAULT);
  socket_vectors vectors;
  if (!vectors.prepare(message->msg_iov, message->msg_iovlen))
    return -1;
  if (message->msg_controllen > std::numeric_limits<ULONG>::max())
    return socket_failure(WSAEINVAL);
  const auto socket = as_socket(descriptor);
  DWORD bytes{}, native_flags = static_cast<DWORD>(flags & ~MSG_DONTWAIT);
  int result{};
  if (message->msg_control && message->msg_controllen) {
    LPFN_WSASENDMSG send{};
    if (!socket_extension(socket, WSAID_WSASENDMSG, send))
      return -1;
    WSAMSG native{
        static_cast<sockaddr*>(message->msg_name),
        message->msg_namelen,
        vectors.data(),
        vectors.count(),
        {static_cast<ULONG>(message->msg_controllen), static_cast<char*>(message->msg_control)},
        0};
    result = send(socket, &native, native_flags, &bytes, nullptr, nullptr);
  } else if (message->msg_name) {
    result = ::WSASendTo(socket,
                         vectors.data(),
                         vectors.count(),
                         &bytes,
                         native_flags,
                         static_cast<sockaddr*>(message->msg_name),
                         message->msg_namelen,
                         nullptr,
                         nullptr);
  } else {
    result =
        ::WSASend(socket, vectors.data(), vectors.count(), &bytes, native_flags, nullptr, nullptr);
  }
  if (result == SOCKET_ERROR)
    return socket_failure();
  return static_cast<std::intptr_t>(bytes);
}
}  // namespace faio::io::windows
#endif
