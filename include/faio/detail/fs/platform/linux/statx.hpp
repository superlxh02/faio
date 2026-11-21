/**
 * @file faio/detail/fs/platform/linux/statx.hpp
 * @brief Linux 元数据转换。
 */
#pragma once
#include <linux/stat.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace faio::fs::detail {
/** @brief 将 statx 属性转换为统一的 stat 快照。 */
inline struct stat stat_from_statx(const struct statx& value) noexcept {
  struct stat result{};
  result.st_dev = ::makedev(value.stx_dev_major, value.stx_dev_minor);
  result.st_ino = value.stx_ino;
  result.st_nlink = value.stx_nlink;
  result.st_mode = value.stx_mode;
  result.st_uid = value.stx_uid;
  result.st_gid = value.stx_gid;
  result.st_rdev = ::makedev(value.stx_rdev_major, value.stx_rdev_minor);
  result.st_size = static_cast<off_t>(value.stx_size);
  result.st_blksize = value.stx_blksize;
  result.st_blocks = value.stx_blocks;
  result.st_atim = {static_cast<time_t>(value.stx_atime.tv_sec),
                    static_cast<long>(value.stx_atime.tv_nsec)};
  result.st_mtim = {static_cast<time_t>(value.stx_mtime.tv_sec),
                    static_cast<long>(value.stx_mtime.tv_nsec)};
  result.st_ctim = {static_cast<time_t>(value.stx_ctime.tv_sec),
                    static_cast<long>(value.stx_ctime.tv_nsec)};
  return result;
}
}  // namespace faio::fs::detail
