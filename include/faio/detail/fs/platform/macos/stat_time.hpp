/**
 * @file faio/detail/fs/platform/macos/stat_time.hpp
 * @brief 原生文件时间转换。
 */
#pragma once
#include <sys/stat.h>

namespace faio::fs::detail {
/** @brief 取得原生文件属性中的修改时间。 */
inline timespec modified_time(const struct stat& value) noexcept {
  return value.st_mtimespec;
}

/** @brief 取得原生文件属性中的访问时间。 */
inline timespec accessed_time(const struct stat& value) noexcept {
  return value.st_atimespec;
}
}  // namespace faio::fs::detail
