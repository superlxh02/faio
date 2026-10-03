#pragma once

namespace faio::io {
/** @brief 实际后端能力快照；原生 Proactor 文件能力与 readiness fallback
 * 分别报告。 */
struct io_capabilities {
  bool network{true}, readiness{true}, vectored{true}, filesystem{true};
  bool native_filesystem{false}, zero_copy{false}, migration{false};
  bool native_accept_nowait{false};  ///< 原生 ACCEPT_DONTWAIT 支持即时批量接收。
  const char* backend{"unavailable"};
};
}  // namespace faio::io
