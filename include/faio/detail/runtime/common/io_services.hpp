/**
 * @file faio/detail/runtime/common/io_services.hpp
 * @brief 运行时共享 IO 辅助服务。
 */
#pragma once
#include "faio/detail/io/core/config.hpp"
#include "faio/detail/io/core/placement.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include <algorithm>
#include <cstddef>
#include <memory>

namespace faio::runtime::detail {
/**
 * @brief 运行时创建相互隔离的服务；readiness 后端预先启动辅助线程，native 后端仅在实际回退时启动。
 */
inline io::engine_config make_io_services(const runtime_config& config) {
  io::engine_config services;
#if defined(__linux__)
  services.requested_backend = config._requested_io_backend;
#endif
  services.native_queue_entries =
      static_cast<unsigned>(std::clamp<std::size_t>(config._num_events, 8, 4096));
  services.max_operations = std::max<std::size_t>(4096, config._num_events);
  services.current_thread_submissions = config._mode == mode::current_thread;

  auto startup = blocking::startup_policy::preheated;
#if defined(__linux__)
  if (io::detail::resolve_io_backend(config._requested_io_backend) == io_backend::IO_URING)
    startup = blocking::startup_policy::on_demand;
#endif
  services.filesystem_service = std::make_shared<blocking::service>(
      config._filesystem_threads, config._filesystem_queue_limit, 1, startup);
  services.resolver_service = std::make_shared<blocking::service>(
      config._resolver_threads, config._resolver_queue_limit, 1, startup);
  services.cleanup_service =
      std::make_shared<blocking::service>(1, config._filesystem_queue_limit, 1, startup);
  services.placement_service = std::make_shared<io::detail::io_placement_group>(
      config._mode == mode::current_thread ? 1 : config._num_workers);
  return services;
}

/**  @brief 在全部 IO shard 停止后回收服务；普通任务排空时调度器仍保持存活。 */
inline void close_io_services(const io::engine_config& services) noexcept {
  if (services.filesystem_service)
    services.filesystem_service->close();
  if (services.resolver_service)
    services.resolver_service->close();
  if (services.cleanup_service)
    services.cleanup_service->close();
}
}  // namespace faio::runtime::detail
