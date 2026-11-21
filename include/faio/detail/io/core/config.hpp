/**
 * @file faio/detail/io/core/config.hpp
 * @brief 配置与容量约束。
 */
#pragma once
#include "faio/detail/blocking/service.hpp"
#include "faio/detail/io/core/fwd.hpp"
#if defined(FAIO_TEST_BACKEND_INJECTION)
#include "faio/detail/io/platform/backend.hpp"
#endif
#if defined(__linux__)
#include "faio/detail/io/platform/linux/backend_selection.hpp"
#endif
#include <cstddef>
#include <memory>
#if defined(__linux__)
#include <optional>
#endif

namespace faio::io {
/**
 *  @brief 独立引擎配置；runtime 可注入共享隔离服务，避免每个 worker
 * 创建线程池。
 */
struct engine_config {
#if defined(FAIO_TEST_BACKEND_INJECTION)
  detail::backend_box (*backend_factory)(){};
  detail::reactor_box (*reactor_factory)(){};
#endif
#if defined(__linux__)
  std::optional<runtime::io_backend> requested_backend;
#endif
  unsigned native_queue_entries{256}; /**< 原生 SQ 容量；与总操作池容量独立。 */
  std::size_t max_operations{4096},
      max_resources{65536}; /**< 同一 IO 域允许登记的长期资源数量上限。 */
  std::size_t filesystem_threads{2}, resolver_threads{1},
      blocking_queue_limit{4096}; /**< 等待中的阻塞任务容量，不含已经执行的任务。 */
  std::shared_ptr<blocking::service> filesystem_service, resolver_service,
      cleanup_service; /**< 共享清理服务租约，专门承担必须排空的关闭任务。 */
  std::shared_ptr<detail::io_placement_group>
      placement_service;             /**< 新连接控制面共享的 shard 弱引用表。 */
  bool current_thread_submissions{}; /**< 由当前线程 runtime 启用的本地 epoll 提交策略。 */
};

}  // namespace faio::io
