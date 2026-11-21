/**
 * @file faio/detail/fs/platform/unix/stat_time.hpp
 * @brief 原生文件时间转换。
 */
#pragma once
#if defined(__APPLE__)
#include "faio/detail/fs/platform/macos/stat_time.hpp"
#else
#include <sys/stat.h>

namespace faio::fs::detail {
/** @brief 取得原生文件属性中的修改时间。 */
inline timespec modified_time(const struct stat& value) noexcept {
  return value.st_mtim;
}

/** @brief 取得原生文件属性中的访问时间。 */
inline timespec accessed_time(const struct stat& value) noexcept {
  return value.st_atim;
}
}  // namespace faio::fs::detail
#endif
