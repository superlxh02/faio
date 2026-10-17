/**
 * @file faio/detail/io/platform/linux/operation_overloads.hpp
 * @brief 跨平台异步 IO：平台扩展操作重载。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#include "faio/detail/io/awaiter/open.hpp"

namespace faio::io {
/** @brief Linux 特有路径解析约束扩展；旧内核返回 ENOSYS，不伪装支持 resolve
 * flags。 */
inline auto open2(const char* path, const open_how* how) {
  return detail::Open2{path, how};
}

inline auto openat2(int directory, const char* path, const open_how* how) {
  return detail::Open2{directory, path, how};
}
}
