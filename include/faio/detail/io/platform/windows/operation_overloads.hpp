/**
 * @file faio/detail/io/platform/windows/operation_overloads.hpp
 * @brief 跨平台异步 IO：平台扩展操作重载。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#include "faio/detail/io/awaiter/open.hpp"
#include "faio/detail/io/awaiter/read.hpp"
#include "faio/detail/io/awaiter/write.hpp"
#include "faio/detail/io/awaiter/readv.hpp"
#include "faio/detail/io/awaiter/writev.hpp"
#include "faio/detail/io/awaiter/fsync.hpp"
#include "faio/detail/io/awaiter/close.hpp"

namespace faio::io {
/** @brief Windows 原生文件 HANDLE 重载；显式转换完整指针位宽。 */
inline auto openat(HANDLE directory, const char* path, int flags, mode_t mode = 0666) {
  return detail::Open{reinterpret_cast<detail::native_descriptor>(directory), path, flags, mode};
}

/// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
inline auto read(HANDLE handle,
                 void* buffer,
                 std::size_t length,
                 std::uint64_t offset = UINT64_MAX) {
  return detail::Read{reinterpret_cast<detail::native_descriptor>(handle), buffer, length, offset};
}

/// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
inline auto write(HANDLE handle,
                  const void* buffer,
                  std::size_t length,
                  std::uint64_t offset = UINT64_MAX) {
  return detail::Write{reinterpret_cast<detail::native_descriptor>(handle), buffer, length, offset};
}

/// @brief 发起分散读取，缓冲区描述及负载在完成前均须有效。
inline auto readv(HANDLE handle,
                  const iovec* vectors,
                  unsigned count,
                  std::uint64_t offset = UINT64_MAX,
                  int flags = 0) {
  return detail::ReadV{
      reinterpret_cast<detail::native_descriptor>(handle), vectors, count, offset, flags};
}

/// @brief 发起聚集写入，按实际完成字节数解释短写。
inline auto writev(HANDLE handle,
                   const iovec* vectors,
                   unsigned count,
                   std::uint64_t offset = UINT64_MAX,
                   int flags = 0) {
  return detail::WriteV{
      reinterpret_cast<detail::native_descriptor>(handle), vectors, count, offset, flags};
}

inline auto fsync(HANDLE handle, unsigned flags = 0) {
  return detail::Fsync{reinterpret_cast<detail::native_descriptor>(handle), flags};
}

/// @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。
inline auto close(HANDLE handle) {
  return detail::Close{handle};
}
}
