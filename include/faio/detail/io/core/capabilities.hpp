/**
 * @file faio/detail/io/core/capabilities.hpp
 * @brief 后端能力报告。
 */
#pragma once

namespace faio::io {
/**
 *  @brief 实际后端能力快照；原生异步文件能力与就绪后端的回退能力分别报告。
 */
struct io_capabilities {
  bool network{true}, readiness{true}, vectored{true},
      filesystem{true}; /**< 是否提供文件系统 IO 接口。 */
  bool native_filesystem{false}, zero_copy{false},
      migration{false};               /**< 是否支持显式迁移资源归属。 */
  bool native_accept_nowait{false};   /**< 原生 ACCEPT_DONTWAIT 支持即时批量接收。 */
  const char* backend{"unavailable"}; /**< 实际启用后端的名称，指向静态字符串。 */
};
}  // namespace faio::io
