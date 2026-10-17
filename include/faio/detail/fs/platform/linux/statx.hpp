/**
 * @file faio/detail/fs/platform/linux/statx.hpp
 * @brief 文件与目录异步接口：Linux 元数据转换。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
/** @brief 已完成 STATX 输出到 POSIX 属性快照的纯转换。 */
#include <linux/stat.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace faio::fs::detail {
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
}
