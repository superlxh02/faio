/**
 * @file faio/detail/runtime/common/config.hpp
 * @brief 配置与容量约束。
 */
#ifndef FAIO_DETAIL_RUNTIME_COMMON_CONFIG_HPP
#define FAIO_DETAIL_RUNTIME_COMMON_CONFIG_HPP

#include "faio/detail/io/platform/linux/backend_selection.hpp"
#include "faio/detail/runtime/common/constants.hpp"
#include "faio/detail/runtime/common/worker_affinity.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace faio::runtime {
enum class mode { current_thread, multi_thread };

/** @brief 用户阻塞任务池的线程生命周期策略。 */
enum class blocking_pool_policy { fixed, elastic };
}  // namespace faio::runtime

namespace faio::runtime::detail {
/**
 *  @brief 空闲轮询次数的自动配置值，在启动线程前解析为所选后端的实际次数。
 * @details Linux 的两个后端在首次空队列驱动后进入完整休眠握手；其他平台执行 32 次有限轮询。
 * 显式配置的 0、4、32 等次数始终优先，不增加工作线程状态，也不增加每次 I/O 的判断。
 */
static inline constexpr uint32_t AUTO_IDLE_SPIN_COUNT{UINT32_MAX};

/** @brief 运行时配置的构建值；runtime_context 在创建线程前验证它。 */
struct runtime_config {
  runtime::mode _mode{
      runtime::mode::multi_thread}; /**< 运行时调度模式，选择当前线程或多工作线程驱动。 */
#if defined(__linux__)
  std::optional<runtime::io_backend> _requested_io_backend;
#endif
  std::size_t _blocking_queue_limit{4096};   /**< 用户阻塞任务等待容量。 */
  std::size_t _filesystem_threads{4};        /**< 文件服务独立线程配额。 */
  std::size_t _resolver_threads{2};          /**< DNS 不占用文件服务线程。 */
  std::size_t _filesystem_queue_limit{4096}; /**< 文件服务等待容量。 */
  std::size_t _resolver_queue_limit{1024};   /**< DNS 服务等待容量。 */
  std::size_t _num_events{
      1024}; /**< 驱动事件批量限制在 1 至 256；原生 SQ 容量与最大在途操作数相互独立。 */
  uint32_t _submit_interval{
      4}; /**< 为兼容保留的配置值；统一驱动每轮执行 flush，readiness 后端在 await 时尝试系统调用。 */
  std::size_t _num_workers{std::max(1u, std::thread::hardware_concurrency())}; /**< 工作线程数量 */
  std::vector<int> _worker_cpus; /**< 空列表不绑定；显式列表与实际 worker 一一对应。 */
  std::size_t _no_stealing_queue_capacity{256}; /**< 每个工作线程的不可窃取队列固定容量。 */
  std::size_t _large_frame_pool_limit_bytes{0}; /**< 运行时大帧空闲池的字节额度；0 表示关闭。 */
#if defined(__linux__)
  std::chrono::nanoseconds _max_io_delay{std::chrono::microseconds(100)};
#else
  std::chrono::nanoseconds _max_io_delay{std::chrono::milliseconds(1)};
#endif
  uint32_t _io_interval{61};           /**< I/O 驱动间隔 */
  uint32_t _global_queue_interval{61}; /**< 全局队列间隔 */
  uint32_t _idle_spin_count{
      AUTO_IDLE_SPIN_COUNT}; /**< 启动前自动解析；零表示首次空队列驱动后直接准备休眠。 */
  std::size_t _max_blocking_threads{64}; /**< 与 I/O worker 分开的阻塞线程上限。 */
  std::chrono::milliseconds _blocking_keep_alive{
      std::chrono::seconds(10)}; /**< 弹性用户阻塞池线程的空闲保活时长。 */
  runtime::blocking_pool_policy _blocking_pool_policy{
      runtime::blocking_pool_policy::elastic}; /**< 用户阻塞任务池的线程生命周期策略。 */
};

/** @brief 验证除数和资源数量；idle_spin_count 为零合法，表示禁用空闲轮询。 */
inline runtime_config validate_config(runtime_config config) {
  if ((config._mode == runtime::mode::multi_thread && config._num_workers == 0)
      || config._num_events == 0 || config._max_io_delay.count() <= 0 || config._io_interval == 0
      || config._global_queue_interval == 0 || config._max_blocking_threads == 0
      || config._blocking_keep_alive.count() <= 0 || !config._blocking_queue_limit
      || !config._filesystem_threads || !config._resolver_threads || !config._filesystem_queue_limit
      || !config._resolver_queue_limit)
    throw std::invalid_argument("运行时线程数、事件数和调度间隔必须大于零");
  if (config._blocking_pool_policy != runtime::blocking_pool_policy::fixed
      && config._blocking_pool_policy != runtime::blocking_pool_policy::elastic)
    throw std::invalid_argument("用户阻塞线程池策略无效");
  if (config._no_stealing_queue_capacity < 2
      || !std::has_single_bit(config._no_stealing_queue_capacity)
      || config._no_stealing_queue_capacity > (std::size_t{1} << 29))
    throw std::invalid_argument("不可窃取队列容量必须是 2..2^29 范围内的 2 的幂");
  validate_worker_affinity(config._worker_cpus,
                           config._mode == runtime::mode::current_thread ? 1 : config._num_workers);
#if defined(__linux__)
  (void)io::detail::resolve_io_backend(config._requested_io_backend);
  if (config._idle_spin_count == AUTO_IDLE_SPIN_COUNT)
    config._idle_spin_count = 0u;
#else
  if (config._idle_spin_count == AUTO_IDLE_SPIN_COUNT)
    config._idle_spin_count = 32u;
#endif
  return config;
}
}  // namespace faio::runtime::detail

namespace faio::runtime {
using config = detail::runtime_config;
/** @brief 公开配置类型的兼容别名。 */
using Config = config;
}  // namespace faio::runtime

namespace std {
/** @brief 运行时配置的格式化适配器。 */
template <>
class formatter<faio::runtime::detail::runtime_config> {
 public:
  /** @brief 解析格式参数。 */
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

  /** @brief 格式化对象状态。 */
  auto format(const faio::runtime::detail::runtime_config& config, auto& context) const noexcept {
    return format_to(
        context.out(),
        R"(mode: {},
                         num_events: {},
                         num_workers: {},
                         worker_cpu_count: {},
                         no_stealing_queue_capacity: {},
                         large_frame_pool_limit_bytes: {},
                         io_interval: {},
                         global_queue_interval: {},
                         submit_interval: {},
                         idle_spin_count: {},
                         max_blocking_threads: {},
                         blocking_pool_policy: {},
                         blocking_keep_alive_ms: {})",
        config._mode == faio::runtime::mode::current_thread ? "current_thread" : "multi_thread",
        config._num_events,
        config._num_workers,
        config._worker_cpus.size(),
        config._no_stealing_queue_capacity,
        config._large_frame_pool_limit_bytes,
        config._io_interval,
        config._global_queue_interval,
        config._submit_interval,
        config._idle_spin_count,
        config._max_blocking_threads,
        config._blocking_pool_policy == faio::runtime::blocking_pool_policy::elastic ? "elastic"
                                                                                     : "fixed",
        config._blocking_keep_alive.count());
  }
};
}  // namespace std

#endif
