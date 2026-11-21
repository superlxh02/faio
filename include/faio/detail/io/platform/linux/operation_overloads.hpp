/**
 * @file faio/detail/io/platform/linux/operation_overloads.hpp
 * @brief 平台扩展操作重载。
 */
#pragma once
#include "faio/detail/io/awaiter/open.hpp"

namespace faio::io {
/**
 *  @brief Linux 特有路径解析约束扩展；旧内核返回 ENOSYS，不伪装支持 resolve
 * flags。
 */
inline auto open2(const char* path, const open_how* how) {
  return detail::Open2{path, how};
}

/** @brief 按目录描述符和路径解析选项打开文件。 */
inline auto openat2(int directory, const char* path, const open_how* how) {
  return detail::Open2{directory, path, how};
}
}  // namespace faio::io
