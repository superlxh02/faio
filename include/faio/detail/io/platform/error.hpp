/**
 * @file faio/detail/io/platform/error.hpp
 * @brief 跨平台异步 IO：错误值与错误域。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#include "faio/detail/common/error.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif

namespace faio::io::detail {
inline Error decode_io_error(int value, std::uint64_t progress = 0) noexcept {
#if defined(_WIN32)
  return windows::make_io_error(value, progress);
#else
  return Error{value, progress};
#endif
}

/** @brief 组合 IO 累加进度；Windows 保留 Win32/Winsock 错误域，不将原生码当 errno。 */
inline Error transfer_error(const Error& error, std::uint64_t progress) noexcept {
#if defined(_WIN32)
  return Error{error.value(), progress, error.domain()};
#else
  return Error{error.value(), progress};
#endif
}
}
