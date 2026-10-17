/**
 * @file faio/detail/io/platform/unix/native_types.hpp
 * @brief 跨平台异步 IO：原生类型与句柄宽度。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/uio.h>

namespace faio::io::detail {
/** @brief POSIX 原生描述符使用系统的 int 表示。 */
using native_descriptor = int;
}
#endif
