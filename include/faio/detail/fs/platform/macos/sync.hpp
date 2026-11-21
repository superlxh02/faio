/**
 * @file faio/detail/fs/platform/macos/sync.hpp
 * @brief 同步协议与数据落盘。
 */
#pragma once
#include <unistd.h>

namespace faio::fs::detail {
/**  @brief macOS 没有 fdatasync；fsync 提供不弱于 data_only 的持久化保证。 */
inline int sync_descriptor_native(int descriptor, bool data_only) noexcept {
  (void)data_only;
  return ::fsync(descriptor);
}
}  // namespace faio::fs::detail
