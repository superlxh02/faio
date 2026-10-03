#pragma once
#include "faio/detail/io/platform/posix_types.hpp"
#include "faio/detail/common/error.hpp"
#if defined(_WIN32)
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <limits>
#include <vector>

namespace faio::io::windows {
/** @brief 将 Winsock 错误映射到引擎内部的 would-block/参数错误分类。
 * @details public Error 仍保留原生 Winsock 编码；本函数仅供控制流判定。
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

/** @brief 同时保存内部 errno 与原生 Winsock 状态，后续不得覆盖 native error。
 * @param error 已取得的原生 Winsock 错误，默认取当前线程的 LastError。
 * @return SOCKET_ERROR；调用方继续使用熟悉的负值失败约定。
 */
inline auto socket_failure(int error = ::WSAGetLastError()) noexcept -> int {
  errno = winsock_errno(error);  // errno 只供内部 would-block/重试分支使用。
  ::WSASetLastError(error);      // 公共 Error 必须从同一个原生值构造，不能丢失错误域。
  return SOCKET_ERROR;
}

/** @brief 进程级 Winsock 2.2 lease；静态初始化线程安全且初始化失败明确可见。 */
inline auto initialize_winsock() noexcept -> expected<void> {
  struct lease {
    int error{};

    lease() noexcept {
      WSADATA data{};  // 清零协商结果，初始化失败时也不会读取不确定状态。
      error = ::WSAStartup(MAKEWORD(2, 2), &data);  // 唯一成功的 startup 持有一个 lease。
      if (!error && data.wVersion != MAKEWORD(2, 2)) {
        // 明确要求 Winsock 2.2。
        ::WSACleanup();              // 协商失败也必须平衡已经成功的 WSAStartup。
        error = WSAVERNOTSUPPORTED;  // 后续调用一致交付原生版本错误。
      }
    }

    ~lease() {
      if (!error)
        ::WSACleanup();
    }
  };

  static lease network;  // inline 函数局部 static 在多 TU 中仍为唯一对象，初始化线程安全。
  if (network.error)     // 失败不抛异常，不创建一个表面有效的 socket。
    return std::unexpected{Error{network.error, 0, error_domain::winsock}};
  return {};
}

/** @brief 显式转换保留所有 handle 位，INVALID_SOCKET 对应 native_descriptor(-1)。 */
inline auto as_socket(detail::native_descriptor descriptor) noexcept -> SOCKET {
  return static_cast<SOCKET>(descriptor);
}

/** @brief socket 始终保持非阻塞与禁止继承，显式 try_* 不会卡住协程 worker。
 * @param descriptor 已由唯一拥有者管理的原生 SOCKET；本函数不取得关闭责任。
 * @return 原生错误域中的失败，或配置成功。
 */
inline auto prepare_socket(detail::native_descriptor descriptor) noexcept -> expected<void> {
  u_long enabled = 1;  // FIONBIO 的参数必须是 Windows u_long，不是 POSIX int。
  if (::ioctlsocket(as_socket(descriptor), FIONBIO, &enabled) == SOCKET_ERROR)
    return std::unexpected{Error{::WSAGetLastError(), 0, error_domain::winsock}};
  // 外部导入的 socket 也统一禁止继承；不能只依赖创建时的 NO_HANDLE_INHERIT。
  if (!::SetHandleInformation(
          reinterpret_cast<HANDLE>(as_socket(descriptor)), HANDLE_FLAG_INHERIT, 0))
    return std::unexpected{Error{static_cast<int>(::GetLastError()), 0, error_domain::win32}};
  return {};
}

/** @brief 读取属于此 socket provider 的扩展函数，不能跨 provider 复用裸指针。 */
template <class Function>
inline auto socket_extension(SOCKET socket, GUID identifier, Function& function) noexcept -> bool {
  DWORD bytes{};  // 输出大小只供 WSAIoctl 使用，不向上层伪装成传输字节。
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
    socket_failure();  // 保存同一个 Winsock 错误供短系统调用返回路径使用。
    return false;
  }
  return function != nullptr;  // 扩展指针只属于本次 socket 的 service provider。
}

/** @brief 短非阻塞分散 IO 的 WSABUF 适配；常见少量向量使用栈存储。
 * @details 只复制描述符，绝不复制 payload；过长元素返回 EINVAL，禁止 ULONG 截断。
 */
class socket_vectors {
 public:
  /** @brief 检查公开 iovec 合同，并建立仅借用 payload 的 WSABUF 数组。 */
  bool prepare(const iovec* vectors, std::size_t count) noexcept {
    if (count > IOV_MAX || (count && !vectors)) {
      // 在读描述符前拒绝空指针和数量溢出。
      socket_failure(count > IOV_MAX ? WSAEINVAL : WSAEFAULT);
      return false;
    }
    try {
      if (count > local_.size()) {
        // 少量向量不分配；较大的合法批次才使用堆存储。
        dynamic_.resize(count);   // 描述符存储在同步 syscall 返回以前始终存在。
        data_ = dynamic_.data();  // WSABUF 不取得 payload 的所有权。
      } else {
        data_ = local_.data();  // 常见一至数个向量直接使用当前栈帧。
      }
      for (std::size_t index = 0; index < count; ++index) {
        // 保留输入向量的顺序和边界。
        if (vectors[index].iov_len > std::numeric_limits<ULONG>::max()
            || (vectors[index].iov_len && !vectors[index].iov_base)) {
          socket_failure(vectors[index].iov_len > std::numeric_limits<ULONG>::max() ? WSAEINVAL
                                                                                    : WSAEFAULT);
          return false;
        }
        data_[index] = {static_cast<ULONG>(vectors[index].iov_len),  // 上面的校验保证窄化无损。
                        static_cast<char*>(vectors[index].iov_base)};
      }
      count_ = static_cast<DWORD>(count);  // IOV_MAX 校验已经保证 DWORD 表示范围。
      // Winsock 要求非空描述符数组；零向量仍表达一条零长度数据报。
      if (!count) {
        local_[0] = {0, &empty_};
        data_ = local_.data();
        count_ = 1;
      }
      return true;
    } catch (...) {
      socket_failure(WSAENOBUFS);  // 分配失败转成原生网络资源错误，不从 noexcept 逃逸。
      return false;
    }
  }

  WSABUF* data() noexcept { return data_; }

  DWORD count() const noexcept { return count_; }

 private:
  std::array<WSABUF, 16> local_{};
  std::vector<WSABUF> dynamic_;
  WSABUF* data_{};
  DWORD count_{};
  char empty_{};
};

/** @brief 短非阻塞 recvmsg；数据报截断保留已经复制的数据与 MSG_TRUNC 信息。
 * @param descriptor 已配置 FIONBIO 的 SOCKET，不得传入普通文件 HANDLE。
 * @param message 输入容量与输出消息元数据；缓冲区保持在调用方拥有范围内。
 * @param flags MSG_DONTWAIT 为库标志，送入原生 API 前剥离；MSG_PEEK 保留消息。
 * @return 实际复制字节，或 -1；截断成功返回复制量并置 MSG_TRUNC/MSG_CTRUNC。
 * @details 不传 OVERLAPPED，返回后没有内核对栈 WSABUF 的借用责任。
 */
inline auto recv_message(detail::native_descriptor descriptor, msghdr* message, int flags) noexcept
    -> std::intptr_t {
  if (!message)
    return socket_failure(WSAEFAULT);  // 在访问消息头前验证裸指针。
  socket_vectors vectors;              // 描述符只在本次短 syscall 中存在，不复制 payload。
  if (!vectors.prepare(message->msg_iov, message->msg_iovlen))
    return -1;
  if (message->msg_controllen > std::numeric_limits<ULONG>::max())
    return socket_failure(WSAEINVAL);
  DWORD bytes{},
      native_flags = static_cast<DWORD>(flags & ~MSG_DONTWAIT);  // 禁止把库私有位传给 Winsock。
  const auto socket = as_socket(descriptor);                     // 完整保留 SOCKET 的所有位。
  int result{};
  if (message->msg_name || message->msg_control) {
    // 地址或辅助数据需要 WSARecvMsg 扩展。
    LPFN_WSARECVMSG receive{};  // 指针从此 socket 的 provider 获取，不能跨 provider 复用。
    if (!socket_extension(socket, WSAID_WSARECVMSG, receive))
      return -1;
    WSAMSG native{
        static_cast<sockaddr*>(message->msg_name),
        message->msg_namelen,
        vectors.data(),
        vectors.count(),
        {static_cast<ULONG>(message->msg_controllen), static_cast<char*>(message->msg_control)},
        native_flags};
    result =
        receive(socket, &native, &bytes, nullptr, nullptr);  // FIONBIO 保证没有 worker 阻塞等待。
    const auto error = result == SOCKET_ERROR ? ::WSAGetLastError() : 0;
    message->msg_namelen = native.namelen;                  // 实际来源地址长度可能小于输入容量。
    message->msg_controllen = native.Control.len;           // 只发布由系统初始化的辅助数据范围。
    message->msg_flags = static_cast<int>(native.dwFlags);  // 保留原生 payload/control 截断区分。
    if (error && error != WSAEMSGSIZE)
      return socket_failure(error);
    if (error == WSAEMSGSIZE && !(native.dwFlags & (MSG_TRUNC | MSG_CTRUNC)))
      message->msg_flags |= MSG_TRUNC;
  } else {
    // 普通 TCP/连接 UDP 消息无需查询 WSARecvMsg 扩展。
    result =
        ::WSARecv(socket, vectors.data(), vectors.count(), &bytes, &native_flags, nullptr, nullptr);
    // Winsock 将 UDP payload 截断报告为 WSAEMSGSIZE，但 bytes 仍是已复制的长度。
    const auto error = result == SOCKET_ERROR ? ::WSAGetLastError() : 0;
    // 真正的网络错误保留原生错误码；截断沿用 recvmsg 的成功 + MSG_TRUNC 合同。
    if (error && error != WSAEMSGSIZE)
      return socket_failure(error);
    message->msg_flags = static_cast<int>(native_flags);
    // WSARecv 没有控制缓冲区，因此 WSAEMSGSIZE 只能表示 payload 截断。
    if (error == WSAEMSGSIZE)
      message->msg_flags |= MSG_TRUNC;
    message->msg_controllen = 0;
  }
  return static_cast<std::intptr_t>(bytes);  // 不发布原始包长，返回值始终是已经复制的范围。
}

/** @brief 短非阻塞 sendmsg；控制数据交给 WSASendMsg，普通消息直接 WSASend(To)。
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
  const auto socket = as_socket(descriptor);  // 转换不将 64 位 SOCKET 窄化成 int。
  DWORD bytes{}, native_flags = static_cast<DWORD>(flags & ~MSG_DONTWAIT);
  int result{};
  if (message->msg_control && message->msg_controllen) {
    // 只有真实辅助数据才需要消息扩展。
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
    // 显式目标地址采用单次 WSASendTo，保留 UDP 包边界。
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
    // 已连接 socket 直接发送，避免不必要的扩展函数查找。
    result =
        ::WSASend(socket, vectors.data(), vectors.count(), &bytes, native_flags, nullptr, nullptr);
  }
  if (result == SOCKET_ERROR)
    return socket_failure();  // 包过大和 would-block 仍是原生失败。
  return static_cast<std::intptr_t>(bytes);
}
}  // namespace faio::io::windows
#endif
