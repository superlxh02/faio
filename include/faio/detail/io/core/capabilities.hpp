/**
 * @file faio/detail/io/core/capabilities.hpp
 * @brief 跨平台异步 IO：后端能力报告。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once

namespace faio::io {
/** @brief 实际后端能力快照；原生 Proactor 文件能力与 readiness fallback
 * 分别报告。 */
struct io_capabilities {
  bool network{true},  ///< 是否提供网络 IO 能力。
      readiness{true},  ///< 是否提供读写就绪观察能力。
      vectored{true},  ///< 是否提供分散读取与聚集写入能力。
      filesystem{true};  ///< 是否提供文件系统 IO 接口。
  bool native_filesystem{false},  ///< 文件操作是否由原生异步后端执行。
      zero_copy{false},  ///< 是否支持零拷贝发送及其负载释放通知。
      migration{false};  ///< 是否支持显式迁移资源归属。
  bool native_accept_nowait{false};  ///< 原生 ACCEPT_DONTWAIT 支持即时批量接收。
  const char* backend{"unavailable"};  ///< 实际启用后端的名称，指向静态字符串。
};
}
