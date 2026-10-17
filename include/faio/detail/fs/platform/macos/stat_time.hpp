/**
 * @file faio/detail/fs/platform/macos/stat_time.hpp
 * @brief 文件与目录异步接口：原生文件时间转换。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include <sys/stat.h>

namespace faio::fs::detail {
inline timespec modified_time(const struct stat& value) noexcept {
  return value.st_mtimespec;
}

inline timespec accessed_time(const struct stat& value) noexcept {
  return value.st_atimespec;
}
}
