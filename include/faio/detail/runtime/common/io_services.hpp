#pragma once
#include "faio/detail/io/engine.hpp"
#include "faio/detail/runtime/common/config.hpp"

namespace faio::runtime::detail {
/** @brief runtime
 * 建立隔离服务；readiness预热，native仅实际fallback才起辅助线程。 */
inline io::engine_config make_io_services(const runtime_config& config) {
  io::engine_config services;
#if defined(__linux__)
  services.requested_backend = config._requested_io_backend;
#endif
  services.native_queue_entries =
      static_cast<unsigned>(std::clamp<std::size_t>(config._num_events, 8, 4096));
  services.max_operations =
      std::max<std::size_t>(4096, config._num_events);  // 事件批量大小不能限制总在途操作数，保留小
  // _num_events 下并发IO。
  auto startup = execution::executor_startup::preheated;
#if defined(__linux__)
  if (io::detail::resolve_io_backend(config._requested_io_backend) == io_backend::IO_URING)
    startup = execution::executor_startup::on_demand;
#endif
  // 仅建立一份服务租约；native数据/目录opcode不提交这些服务、也不启动其线程。
  services.filesystem_service = std::make_shared<execution::blocking_executor>(
      config._filesystem_threads, config._filesystem_queue_limit, 1, startup);
  services.resolver_service = std::make_shared<execution::blocking_executor>(
      config._resolver_threads, config._resolver_queue_limit, 1, startup);
  services.cleanup_service =
      std::make_shared<execution::blocking_executor>(1, config._filesystem_queue_limit, 1, startup);
  services.placement_service = std::make_shared<io::detail::io_placement_group>(
      config._mode == mode::current_thread ? 1 : config._num_workers);
  return services;
}

/** @brief 在全部 IO shard 停止后回收服务；普通任务排空时调度器仍保持存活。 */
inline void close_io_services(const io::engine_config& services) noexcept {
  if (services.filesystem_service)
    services.filesystem_service->close();
  if (services.resolver_service)
    services.resolver_service->close();
  if (services.cleanup_service)
    services.cleanup_service->close();
}
}  // namespace faio::runtime::detail
