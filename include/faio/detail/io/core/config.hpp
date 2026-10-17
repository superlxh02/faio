/**
 * @file faio/detail/io/core/config.hpp
 * @brief 跨平台异步 IO：配置与容量约束。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/execution/blocking_executor.hpp"
#include "faio/detail/io/core/fwd.hpp"
#include "faio/detail/io/platform/backend.hpp"
#include "faio/detail/io/platform/linux/backend_selection.hpp"
#include <cstddef>
#include <memory>
#include <optional>

namespace faio::io {
/** @brief 独立引擎配置；runtime 可注入共享隔离服务，避免每个 worker
 * 创建线程池。 */
struct engine_config {
#if defined(FAIO_TEST_BACKEND_INJECTION)
  detail::backend_box (*backend_factory)(){};  ///< 测试构建注入的后端工厂；生产构建不包含此接入点。
  detail::reactor_box (*reactor_factory)(){};  ///< 测试构建注入的就绪反应器工厂，用于确定性验证后端契约。
#endif
#if defined(__linux__)
  std::optional<runtime::io_backend>
      requested_backend;  ///< 显式后端选择必须与编译期实现匹配，未指定时使用固定默认后端。
#endif
  unsigned native_queue_entries{256};  ///< 原生 SQ 容量；与总操作池容量独立。
  std::size_t max_operations{4096},  ///< 预分配的稳定操作槽数量上限。
      max_resources{65536};  ///< 同一 IO 域允许登记的长期资源数量上限。
  std::size_t filesystem_threads{2},  ///< 文件阻塞服务的工作线程数量。
      resolver_threads{1},  ///< 地址解析阻塞服务的工作线程数量。
      blocking_queue_limit{4096};  ///< 等待中的阻塞任务容量，不含已经执行的任务。
  std::shared_ptr<execution::blocking_executor> filesystem_service,  ///< 共享文件阻塞服务租约，未提供时由域创建。
      resolver_service,  ///< 共享地址解析服务租约，未提供时由域创建。
      cleanup_service;  ///< 共享清理服务租约，专门承担必须排空的关闭任务。
  std::shared_ptr<detail::io_placement_group> placement_service;  ///< 新连接控制面共享弱 shard 表。
};

}
