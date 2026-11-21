/**
 * @file faio/detail/io/platform/unix/native_types.hpp
 * @brief 原生类型与句柄宽度。
 */
#pragma once
#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/uio.h>

namespace faio::io::detail {
/**  @brief POSIX 原生描述符使用系统的 int 表示。 */
using native_descriptor = int;
}  // namespace faio::io::detail
#endif
