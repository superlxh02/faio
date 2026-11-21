/**
 * @file faio/detail/fs/platform/unix/sync.hpp
 * @brief 同步协议与数据落盘。
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
/** @brief 使用平台同步接口刷出文件数据或完整元数据。 */
inline int sync_descriptor_native(int descriptor, bool data_only) noexcept {
  return data_only ? ::fdatasync(descriptor) : ::fsync(descriptor);
}
#endif

/** @brief 根据平台能力同步文件描述符。 */
inline expected<void> sync_descriptor(int descriptor, bool data_only) noexcept {
  int result;
  do {
    result = sync_descriptor_native(descriptor, data_only);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return std::unexpected{make_error(errno)};
  return {};
}
}  // namespace faio::fs::detail
