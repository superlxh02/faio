/**
 * @file faio/detail/fs/platform/macos/sync.hpp
 * @brief 文件与目录异步接口：同步协议与数据落盘。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include <unistd.h>

namespace faio::fs::detail {
/** @brief macOS 没有 fdatasync；fsync 提供不弱于 data_only 的持久化保证。 */
inline int sync_descriptor_native(int descriptor, bool data_only) noexcept {
  (void)data_only;
  return ::fsync(descriptor);
}
}
