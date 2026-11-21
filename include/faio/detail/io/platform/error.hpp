/**
 * @file faio/detail/io/platform/error.hpp
 * @brief 错误值与错误域。
 */
#pragma once
#include "faio/detail/common/error.hpp"
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#endif

namespace faio::io::detail {
/** @brief 将原生 I/O 错误转换为错误结果。 */
inline Error decode_io_error(int value, std::uint64_t progress = 0) noexcept {
#if defined(_WIN32)
  return windows::make_io_error(value, progress);
#else
  return Error{value, progress};
#endif
}

/**  @brief 组合 IO 累加进度；Windows 保留 Win32/Winsock 错误域，不将原生码当 errno。 */
inline Error transfer_error(const Error& error, std::uint64_t progress) noexcept {
  return error.with_progress(progress);
}
}  // namespace faio::io::detail
