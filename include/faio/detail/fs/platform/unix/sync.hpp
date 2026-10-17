/**
 * @file faio/detail/fs/platform/unix/sync.hpp
 * @brief 文件与目录异步接口：同步协议与数据落盘。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include "faio/detail/common/error.hpp"
#include <cerrno>
#include <unistd.h>
#if defined(__APPLE__)
#include "faio/detail/fs/platform/macos/sync.hpp"
#endif

namespace faio::fs::detail {
#if !defined(__APPLE__)
inline int sync_descriptor_native(int descriptor, bool data_only) noexcept {
  return data_only ? ::fdatasync(descriptor) : ::fsync(descriptor);
}
#endif

inline expected<void> sync_descriptor(int descriptor, bool data_only) noexcept {
  int result;
  do {
    result = sync_descriptor_native(descriptor, data_only);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return std::unexpected{make_error(errno)};
  return {};
}
}
