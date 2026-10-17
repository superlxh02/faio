/**
 * @file faio/detail/io/awaiter/operations.hpp
 * @brief 跨平台异步 IO：操作工厂与组合接口。
 * @details 构造阶段只保存参数，真正准备与提交发生在挂起阶段。借用负载与输出存储须保持到安全完成，立即完成和跨线程完成通过统一握手避免重复恢复。
 */
#pragma once
#include "faio/detail/io/awaiter/accept.hpp"
#include "faio/detail/io/awaiter/cancel.hpp"
#include "faio/detail/io/awaiter/close.hpp"
#include "faio/detail/io/awaiter/cmd_sock.hpp"
#include "faio/detail/io/awaiter/connect.hpp"
#include "faio/detail/io/awaiter/fsync.hpp"
#include "faio/detail/io/awaiter/open.hpp"
#include "faio/detail/io/awaiter/read.hpp"
#include "faio/detail/io/awaiter/readv.hpp"
#include "faio/detail/io/awaiter/recv.hpp"
#include "faio/detail/io/awaiter/recvfrom.hpp"
#include "faio/detail/io/awaiter/recvmsg.hpp"
#include "faio/detail/io/awaiter/send.hpp"
#include "faio/detail/io/awaiter/sendmsg.hpp"
#include "faio/detail/io/awaiter/sendto.hpp"
#include "faio/detail/io/awaiter/shutdown.hpp"
#include "faio/detail/io/awaiter/socket.hpp"
#include "faio/detail/io/awaiter/write.hpp"
#include "faio/detail/io/awaiter/writev.hpp"
#include "faio/detail/io/engine.hpp"

#include "faio/detail/io/awaiter/ready.hpp"

namespace faio::io {
/// @brief 兼容 raw fd 及现代稳定资源重载；平台类型不透过 liburing 进入公共头。
template <class F>
inline auto accept(F fd, sockaddr* address, socklen_t* length, int flags = 0) {
  return detail::Accept{std::move(fd), address, length, flags};
}

template <class F>
inline auto cancel(F fd, unsigned flags = 0) {
  return detail::Cancel{std::move(fd), flags};
}

/// @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。
template <class F>
inline auto close(F fd) {
  return detail::Close{std::move(fd)};
}

template <class F>
inline auto connect(F fd, const sockaddr* address, socklen_t length) {
  return detail::Connect{std::move(fd), address, length};
}

template <class F>
inline auto fsync(F fd, unsigned flags = 0) {
  return detail::Fsync{std::move(fd), flags};
}

inline auto open(const char* path, int flags, mode_t mode = 0666) {
  return detail::Open{path, flags, mode};
}

inline auto openat(detail::native_descriptor directory,
                   const char* path,
                   int flags,
                   mode_t mode = 0666) {
  return detail::Open{directory, path, flags, mode};
}

/// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
template <class F>
inline auto read(F fd, void* buffer, std::size_t length, std::uint64_t offset = UINT64_MAX) {
  return detail::Read{std::move(fd), buffer, length, offset};
}

/// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
template <class F>
inline auto write(F fd, const void* buffer, std::size_t length, std::uint64_t offset = UINT64_MAX) {
  return detail::Write{std::move(fd), buffer, length, offset};
}

/// @brief 发起分散读取，缓冲区描述及负载在完成前均须有效。
template <class F>
inline auto readv(
    F fd, const iovec* vectors, unsigned count, std::uint64_t offset = UINT64_MAX, int flags = 0) {
  return detail::ReadV{std::move(fd), vectors, count, offset, flags};
}

/// @brief 发起聚集写入，按实际完成字节数解释短写。
template <class F>
inline auto writev(
    F fd, const iovec* vectors, unsigned count, std::uint64_t offset = UINT64_MAX, int flags = 0) {
  return detail::WriteV{std::move(fd), vectors, count, offset, flags};
}

/// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
template <class F>
inline auto recv(F fd, void* buffer, std::size_t length, int flags = 0) {
  return detail::Recv{std::move(fd), buffer, length, flags};
}

/// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
template <class F>
inline auto send(F fd, const void* buffer, std::size_t length, int flags = 0) {
  return detail::Send{std::move(fd), buffer, length, flags};
}

template <class F>
inline auto recvfrom(F fd,
                     void* buffer,
                     std::size_t length,
                     int flags,
                     sockaddr* address,
                     socklen_t* address_length) {
  return detail::RecvFrom{std::move(fd), buffer, length, flags, address, address_length};
}

template <class F>
inline auto sendto(F fd,
                   const void* buffer,
                   std::size_t length,
                   int flags,
                   const sockaddr* address,
                   socklen_t address_length) {
  return detail::SendTo{std::move(fd), buffer, length, flags, address, address_length};
}

template <class F>
inline auto recvmsg(F fd, msghdr* message, unsigned flags = 0) {
  return detail::RecvMsg{std::move(fd), message, flags};
}

template <class F>
inline auto sendmsg(F fd, const msghdr* message, unsigned flags = 0) {
  return detail::SendMsg{std::move(fd), message, flags};
}

/// @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。
template <class F>
inline auto shutdown(F fd, int how) {
  return detail::Shutdown{std::move(fd), how};
}

inline auto socket(int family, int type, int protocol, unsigned flags = 0) {
  return detail::Socket{family, type, protocol, flags};
}

template <class F>
inline auto ready(F fd, Interest interest) {
  return detail::ReadyAwaiter{std::move(fd), interest};
}

inline auto getsockopt(
    detail::native_descriptor fd, int level, int option, void* value, int length) {
  return detail::CmdSock{detail::socket_getsockopt_command, fd, level, option, value, length};
}

inline auto setsockopt(
    detail::native_descriptor fd, int level, int option, void* value, int length) {
  return detail::CmdSock{detail::socket_setsockopt_command, fd, level, option, value, length};
}

inline auto cmdsock(
    int command, detail::native_descriptor fd, int level, int option, void* value, int length) {
  return detail::CmdSock{command, fd, level, option, value, length};
}

template <class F>
inline auto send_zc(
    F fd, const void* buffer, std::size_t length, int flags, unsigned zc_flags = 0) {
  return detail::SendZC{std::move(fd), buffer, length, flags, zc_flags};
}

template <class F>
inline auto sendmsg_zc(F fd, const msghdr* message, unsigned flags = 0) {
  return detail::SendMsgZC{std::move(fd), message, flags};
}
}

#if defined(_WIN32)
#include "faio/detail/io/platform/windows/operation_overloads.hpp"
#endif
#if defined(__linux__)
#include "faio/detail/io/platform/linux/operation_overloads.hpp"
#endif
