/**
 * @file faio/detail/io/platform/windows/native_types.hpp
 * @brief 原生类型与句柄宽度。
 */
#pragma once
#if defined(_WIN32)
#include <cstddef>
#include <cstdint>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <sys/types.h>

/**  @brief 地址长度保持 Winsock 的有符号 int；负长度必须在提交前拒绝。 */
using socklen_t = int;
#if defined(_MSC_VER)
/**  @brief 保留 raw open API 的权限参数；Windows 权限由 ACL 决定。 */
using mode_t = unsigned short;
#endif
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
/**  @brief 平台中立分散缓冲描述；只是描述符，不拥有 payload。 */
struct iovec {
  void* iov_base{};      /**< 内核访问的借用数据起点。 */
  std::size_t iov_len{}; /**< 字节长度，转换 WSABUF 前检查 ULONG 上限。 */
};

/**
 *  @brief 保持现有网络 public API 的消息描述；后端显式转换为 WSAMSG。
 * @details 此布局不是 WSAMSG，绝不 reinterpret_cast；控制数据使用
 *          Windows 原生 WSACMSGHDR 的格式。
 */
struct msghdr {
  void* msg_name{};             /**< 可选目标或来源 sockaddr。 */
  socklen_t msg_namelen{};      /**< 地址借用容量或真实返回长度。 */
  iovec* msg_iov{};             /**< 借用的分散/聚集描述符数组。 */
  std::size_t msg_iovlen{};     /**< 数组元素数。 */
  void* msg_control{};          /**< Windows 原生控制消息缓冲区。 */
  std::size_t msg_controllen{}; /**< 控制区容量或真实返回字节数。 */
  int msg_flags{};              /**< MSG_TRUNC/MSG_CTRUNC 等完成标志。 */
};

using cmsghdr = WSACMSGHDR;
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40000000
#endif
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x10000000
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x20000000
#endif
#ifndef CMSG_SPACE
#define CMSG_SPACE(length) WSA_CMSG_SPACE(length)
#define CMSG_LEN(length) WSA_CMSG_LEN(length)
#define CMSG_DATA(header) WSA_CMSG_DATA(header)
#endif
/** @brief 取得首个原生辅助消息头。 */
inline auto faio_cmsg_firsthdr(const msghdr* message) noexcept -> cmsghdr* {
  return message && message->msg_control && message->msg_controllen >= sizeof(cmsghdr)
             ? static_cast<cmsghdr*>(message->msg_control)
             : nullptr;
}

/** @brief 取得下一个原生辅助消息头。 */
inline auto faio_cmsg_nxthdr(const msghdr* message, const cmsghdr* header) noexcept -> cmsghdr* {
  if (!message || !header || !message->msg_control || header->cmsg_len < sizeof(cmsghdr))
    return nullptr;
  const auto begin = reinterpret_cast<std::uintptr_t>(message->msg_control);
  const auto current = reinterpret_cast<std::uintptr_t>(header);
  if (current < begin || current - begin > message->msg_controllen)
    return nullptr;
  const auto offset = static_cast<std::size_t>(current - begin);
  const auto remaining = message->msg_controllen - offset;
  if (header->cmsg_len > remaining)
    return nullptr;
  const auto aligned = WSA_CMSGHDR_ALIGN(header->cmsg_len);
  if (aligned < header->cmsg_len || aligned > remaining || remaining - aligned < sizeof(cmsghdr))
    return nullptr;
  return reinterpret_cast<cmsghdr*>(current + aligned);
}

#ifdef CMSG_FIRSTHDR
#undef CMSG_FIRSTHDR
#endif
#ifdef CMSG_NXTHDR
#undef CMSG_NXTHDR
#endif
#define CMSG_FIRSTHDR(message) faio_cmsg_firsthdr(message)
#define CMSG_NXTHDR(message, header) faio_cmsg_nxthdr(message, header)

namespace faio::io::detail {
/**  @brief Windows 原生描述符保留 SOCKET/HANDLE 的完整指针宽度。 */
using native_descriptor = std::intptr_t;
}  // namespace faio::io::detail
#endif
