/**
 * @file faio/detail/runtime/common/config.hpp
 * @brief 运行时调度、线程协调与定时驱动：配置与容量约束。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#ifndef FAIO_DETAIL_RUNTIME_COMMON_CONFIG_HPP
#define FAIO_DETAIL_RUNTIME_COMMON_CONFIG_HPP

#include "faio/detail/io/platform/linux/backend_selection.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <thread>

namespace faio::runtime {
enum class mode { current_thread, multi_thread };
}

namespace faio::runtime::detail {
static inline constexpr std::size_t MAX_LEVEL{6uz};
static inline constexpr std::size_t SLOT_SIZE{64uz};
/// @brief 槽位数量以 2 为底的对数。
static inline constexpr std::size_t SLOT_SHIFT{6uz};
/// @brief 槽位索引的位掩码。
static inline constexpr std::size_t SLOT_MASK{SLOT_SIZE - 1uz};
static inline constexpr std::size_t LOCAL_QUEUE_CAPACITY{256uz};
/** @brief 空闲轮询次数的自动配置值，在启动线程前解析为所选后端的实际次数。
 * @details Linux 双后端在首次空队列驱动后进入完整休眠握手；其他平台为 32
 * 次有限轮询。 显式配置 0/4/32 等次数始终优先，不增加 worker 状态或每次 IO
 * 的判断。
 */
static inline constexpr uint32_t AUTO_IDLE_SPIN_COUNT{UINT32_MAX};

/// @brief 运行时配置的构建值；runtime_context 在创建线程前验证它。
struct runtime_config {
  runtime::mode _mode{runtime::mode::multi_thread};  ///< 运行时调度模式，选择当前线程或多工作线程驱动。
#if defined(__linux__)
  std::optional<runtime::io_backend>
      _requested_io_backend;  ///< 只校验显式选择与编译期后端一致，不在运行期切换后端类型。
#endif
  std::size_t _blocking_queue_limit{4096};  ///< 用户阻塞任务等待容量。
  std::size_t _filesystem_threads{4};  ///< 文件服务独立线程配额。
  std::size_t _resolver_threads{2};  ///< DNS 不占用文件服务线程。
  std::size_t _filesystem_queue_limit{4096};  ///< 文件服务等待容量。
  std::size_t _resolver_queue_limit{1024};  ///< DNS 服务等待容量。
  std::size_t _num_events{1024};  ///< 驱动事件批量截取到1..256；原生SQ容量与最大在途操作数独立。
  uint32_t _submit_interval{
      4};  ///< 兼容配置值；当前统一驱动每轮flush，readiness在await时尝试syscall。
  std::size_t _num_workers{std::max(1u, std::thread::hardware_concurrency())};  ///< 工作线程数量
#if defined(__linux__)
  std::chrono::nanoseconds _max_io_delay{
      std::chrono::microseconds(100)};  ///< 持续执行就绪任务时允许推迟 IO 驱动的时间预算，在可恢复边界检查。
#else
  std::chrono::nanoseconds _max_io_delay{
      std::chrono::milliseconds(1)};  ///< 持续执行就绪任务时允许推迟 IO 驱动的时间预算，在可恢复边界检查。
#endif
  uint32_t _io_interval{61};  ///< io间隔
  uint32_t _global_queue_interval{61};  ///< 全局队列间隔
  uint32_t _idle_spin_count{
      AUTO_IDLE_SPIN_COUNT};  ///< 启动前自动解析；零表示首次空队列驱动后直接准备休眠。
  std::size_t _max_blocking_threads{64};  ///< 与 I/O worker 分开的阻塞线程上限。
  std::chrono::milliseconds _blocking_keep_alive{std::chrono::seconds(10)};  ///< 空闲阻塞线程的保活时长配置。
};

/// @brief 验证除数和资源数量；idle_spin_count 为零合法，表示禁用空闲轮询。
inline runtime_config validate_config(runtime_config config) {
  if ((config._mode == runtime::mode::multi_thread && config._num_workers == 0)
      || config._num_events == 0 || config._max_io_delay.count() <= 0 || config._io_interval == 0
      || config._global_queue_interval == 0 || config._max_blocking_threads == 0
      || config._blocking_keep_alive.count() <= 0 || !config._blocking_queue_limit
      || !config._filesystem_threads || !config._resolver_threads || !config._filesystem_queue_limit
      || !config._resolver_queue_limit)
    throw std::invalid_argument("运行时线程数、事件数和调度间隔必须大于零");
#if defined(__linux__)
  (void)io::detail::resolve_io_backend(config._requested_io_backend);
  /// @brief epoll/uring 都保留首次真实驱动与完整等待握手。
  if (config._idle_spin_count == AUTO_IDLE_SPIN_COUNT)
    config._idle_spin_count = 0u;
#else
  /// @brief kqueue 等平台的既有策略在配置边界一次解析。
  if (config._idle_spin_count == AUTO_IDLE_SPIN_COUNT)
    config._idle_spin_count = 32u;
#endif
  return config;
}
}

namespace std {
template <>
class formatter<faio::runtime::detail::runtime_config> {
 public:
  constexpr auto parse(format_parse_context& context) {
    auto it{context.begin()};
    auto end{context.end()};
    if (it == end || *it == '}') {
      return it;
    }
    ++it;
    if (it != end && *it != '}') {
      throw format_error("Invalid format specifier for runtime_config");
    }
    return it;
  }

  auto format(const faio::runtime::detail::runtime_config& config, auto& context) const noexcept {
    return format_to(
        context.out(),
        R"(mode: {},
                         num_events: {},
                         num_workers: {},
                         io_interval: {},
                         global_queue_interval: {},
                         submit_interval: {},
                         idle_spin_count: {},
                         max_blocking_threads: {},
                         blocking_keep_alive_ms: {})",
        config._mode == faio::runtime::mode::current_thread ? "current_thread" : "multi_thread",
        config._num_events,
        config._num_workers,
        config._io_interval,
        config._global_queue_interval,
        config._submit_interval,
        config._idle_spin_count,
        config._max_blocking_threads,
        config._blocking_keep_alive.count());
  }
};
}

#endif
