/**
 * @file faio/detail/experimental/runtime_options.hpp
 * @brief 运行时选项。
 */
#ifndef FAIO_DETAIL_EXPERIMENTAL_RUNTIME_OPTIONS_HPP
#define FAIO_DETAIL_EXPERIMENTAL_RUNTIME_OPTIONS_HPP

#include "faio/detail/experimental/platform.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

namespace faio::runtime {
using config = detail::runtime_config;
}

namespace faio::experimental {
enum class entry_exit_policy { cancel_and_drain, drain };

/** @brief GCC 反射注解要求结构化值，而 std::optional 含有私有状态；这里使用成员公开的字面量类型保留可选值语义。 */
struct io_backend_option {
  bool present{}; /**< 是否显式设置后端选项；false 表示沿用默认选择。 */
  faio::runtime::io_backend stored{
      faio::runtime::io_backend::IO_EPOLL}; /**< 公开存储后端枚举值，使其可用于结构化反射注解。 */
  /** @brief 创建未指定 I/O 后端的选项。 */
  constexpr io_backend_option() noexcept = default;

  /** @brief 创建未指定 I/O 后端的选项。 */
  constexpr io_backend_option(std::nullopt_t) noexcept {}

  /** @brief 创建显式指定 I/O 后端的选项。 */
  constexpr io_backend_option(faio::runtime::io_backend value) noexcept
      : present(true), stored(value) {}

  /** @brief 从可选后端构造结构化注解值。 */
  constexpr io_backend_option(std::optional<faio::runtime::io_backend> value) noexcept
      : present(value.has_value()), stored(value.value_or(faio::runtime::io_backend::IO_EPOLL)) {}

  /** @brief 检查是否显式设置了 I/O 后端。 */
  constexpr explicit operator bool() const noexcept { return present; }

  /** @brief 检查是否显式设置了 I/O 后端。 */
  constexpr bool has_value() const noexcept { return present; }

  /** @brief 读取已存储的 I/O 后端，调用方须保证选项非空。 */
  constexpr faio::runtime::io_backend operator*() const noexcept { return stored; }

  /** @brief 读取 I/O 后端；选项为空时抛出 bad_optional_access。 */
  constexpr faio::runtime::io_backend value() const {
    if (!present)
      throw std::bad_optional_access{};
    return stored;
  }

  /** @brief 返回已设置的 I/O 后端，未设置时返回给定的默认值。 */
  constexpr faio::runtime::io_backend value_or(faio::runtime::io_backend fallback) const noexcept {
    return present ? stored : fallback;
  }

  /** @brief 转换成标准可选后端值。 */
  constexpr operator std::optional<faio::runtime::io_backend>() const noexcept {
    return present ? std::optional{stored} : std::nullopt;
  }

  /** @brief 清空后端选项，使其恢复为未指定状态；可重复调用。 */
  constexpr void reset() noexcept {
    present = false;
    stored = faio::runtime::io_backend::IO_EPOLL;
  }

  /** @brief 比较两个选项是否均为空，或是否保存了相同的后端值。 */
  friend constexpr bool operator==(io_backend_option left, io_backend_option right) noexcept {
    return left.present == right.present && (!left.present || left.stored == right.stored);
  }
};

/** @brief 用于配置转换和反射注解的结构化运行时选项。 */
struct runtime_options {
  faio::runtime::mode mode{
      faio::runtime::mode::multi_thread};  /**< 运行时调度模式，选择当前线程或多工作线程驱动。 */
  std::size_t workers{};                   /**< 工作线程数量，零值由配置校验解析为默认数量。 */
  std::uint32_t io_interval{61};           /**< 两次周期 IO 驱动之间的调度轮数，必须非零。 */
  std::uint32_t global_queue_interval{61}; /**< 全局队列公平性检查间隔，单位为调度轮数。 */
  std::uint32_t idle_spin_count{
      faio::runtime::detail::
          AUTO_IDLE_SPIN_COUNT}; /**< 空闲搜索线程进入内核等待前的自旋轮数上限。 */
  std::size_t num_events{1024};  /**< 一次后端事件轮询可接收的事件数量上限。 */
  std::uint64_t max_io_delay_ns{
      100000}; /**< 持续执行就绪任务时允许的最大 IO 驱动延迟，单位为纳秒。 */
  std::size_t max_blocking_threads{64};        /**< 运行时通用阻塞任务池的最大线程数。 */
  std::uint64_t blocking_keep_alive_ms{10000}; /**< 空闲阻塞线程的保活时长，单位为毫秒。 */
  std::size_t blocking_queue_limit{
      4096};                         /**< 等待执行的阻塞任务数量上限，不含已经开始执行的任务。 */
  std::size_t filesystem_threads{4}; /**< 文件阻塞服务的工作线程数量。 */
  std::size_t resolver_threads{2};   /**< 地址解析阻塞服务的工作线程数量。 */
  std::size_t filesystem_queue_limit{4096};   /**< 文件阻塞服务允许排队的任务数上限。 */
  std::size_t resolver_queue_limit{1024};     /**< 地址解析服务允许排队的任务数上限。 */
  io_backend_option io_backend{std::nullopt}; /**< 显式后端选择；未指定时采用编译期默认后端。 */
  entry_exit_policy exit_policy{
      entry_exit_policy::cancel_and_drain}; /**< 入口任务返回后对派生任务采用的取消或排空策略。 */
};

enum class options_error {
  none,
  mode,
  workers,
  io_interval,
  global_queue_interval,
  num_events,
  max_io_delay_ns,
  max_blocking_threads,
  blocking_keep_alive_ms,
  blocking_queue_limit,
  filesystem_threads,
  resolver_threads,
  filesystem_queue_limit,
  resolver_queue_limit,
  io_backend,
  io_uring_unavailable,
  exit_policy
};

namespace detail {
/** @brief 检查无符号时长能否由目标计数类型表示。 */
template <class Rep>
constexpr bool duration_representable(std::uint64_t value) noexcept {
  return value <= static_cast<std::uint64_t>(std::numeric_limits<Rep>::max());
}

/** @brief 返回运行时选项校验错误的说明。 */
constexpr const char* options_error_message(options_error error) noexcept {
  switch (error) {
    case options_error::none:
      return "faio experimental: valid runtime_options";
    case options_error::mode:
      return "faio experimental: invalid mode";
    case options_error::workers:
      return "faio experimental: invalid workers";
    case options_error::io_interval:
      return "faio experimental: invalid io_interval";
    case options_error::global_queue_interval:
      return "faio experimental: invalid global_queue_interval";
    case options_error::num_events:
      return "faio experimental: invalid num_events";
    case options_error::max_io_delay_ns:
      return "faio experimental: invalid max_io_delay_ns";
    case options_error::max_blocking_threads:
      return "faio experimental: invalid max_blocking_threads";
    case options_error::blocking_keep_alive_ms:
      return "faio experimental: invalid blocking_keep_alive_ms";
    case options_error::blocking_queue_limit:
      return "faio experimental: invalid blocking_queue_limit";
    case options_error::filesystem_threads:
      return "faio experimental: invalid filesystem_threads";
    case options_error::resolver_threads:
      return "faio experimental: invalid resolver_threads";
    case options_error::filesystem_queue_limit:
      return "faio experimental: invalid filesystem_queue_limit";
    case options_error::resolver_queue_limit:
      return "faio experimental: invalid resolver_queue_limit";
    case options_error::io_backend:
      return "faio experimental: invalid io_backend";
    case options_error::io_uring_unavailable:
      return "faio experimental: io_uring not compiled";
    case options_error::exit_policy:
      return "faio experimental: invalid exit_policy";
  }
  return "faio experimental: invalid runtime_options";
}
}  // namespace detail

/** @brief 校验运行时选项并返回首个错误类别。 */
constexpr options_error validate_options(const runtime_options& options) noexcept {
  if (options.mode != faio::runtime::mode::multi_thread
      && options.mode != faio::runtime::mode::current_thread)
    return options_error::mode;
  if (options.exit_policy != entry_exit_policy::cancel_and_drain
      && options.exit_policy != entry_exit_policy::drain)
    return options_error::exit_policy;
  if (options.mode == faio::runtime::mode::current_thread && options.workers > 1)
    return options_error::workers;
  if (!options.io_interval)
    return options_error::io_interval;
  if (!options.global_queue_interval)
    return options_error::global_queue_interval;
  if (!options.num_events)
    return options_error::num_events;
  if (!options.max_io_delay_ns
      || !detail::duration_representable<std::chrono::nanoseconds::rep>(options.max_io_delay_ns))
    return options_error::max_io_delay_ns;
  if (!options.max_blocking_threads)
    return options_error::max_blocking_threads;
  if (!options.blocking_keep_alive_ms
      || !detail::duration_representable<std::chrono::milliseconds::rep>(
          options.blocking_keep_alive_ms))
    return options_error::blocking_keep_alive_ms;
  if (!options.blocking_queue_limit)
    return options_error::blocking_queue_limit;
  if (!options.filesystem_threads)
    return options_error::filesystem_threads;
  if (!options.resolver_threads)
    return options_error::resolver_threads;
  if (!options.filesystem_queue_limit)
    return options_error::filesystem_queue_limit;
  if (!options.resolver_queue_limit)
    return options_error::resolver_queue_limit;
  if (options.io_backend) {
    if (*options.io_backend != faio::runtime::io_backend::IO_EPOLL
        && *options.io_backend != faio::runtime::io_backend::IO_URING)
      return options_error::io_backend;
#if !defined(FAIO_HAS_IO_URING) || !FAIO_HAS_IO_URING
    if (*options.io_backend == faio::runtime::io_backend::IO_URING)
      return options_error::io_uring_unavailable;
#endif
  }
  return options_error::none;
}

/** @brief 校验选项并转换成稳定运行时配置。 */
inline faio::runtime::config to_runtime_config(runtime_options options) {
  if (const auto error = validate_options(options); error != options_error::none)
    throw std::invalid_argument(detail::options_error_message(error));
  faio::runtime::config config{};
  config._mode = options.mode;
  config._num_workers = options.mode == faio::runtime::mode::current_thread ? 1
                        : options.workers                                   ? options.workers
                                          : std::max(1u, std::thread::hardware_concurrency());
  config._io_interval = options.io_interval;
  config._global_queue_interval = options.global_queue_interval;
  config._idle_spin_count = options.idle_spin_count;
  config._num_events = options.num_events;
  config._max_io_delay =
      std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(options.max_io_delay_ns)};
  config._max_blocking_threads = options.max_blocking_threads;
  config._blocking_keep_alive = std::chrono::milliseconds{
      static_cast<std::chrono::milliseconds::rep>(options.blocking_keep_alive_ms)};
  config._blocking_queue_limit = options.blocking_queue_limit;
  config._filesystem_threads = options.filesystem_threads;
  config._resolver_threads = options.resolver_threads;
  config._filesystem_queue_limit = options.filesystem_queue_limit;
  config._resolver_queue_limit = options.resolver_queue_limit;
  config._requested_io_backend =
      static_cast<std::optional<faio::runtime::io_backend>>(options.io_backend);
  return faio::runtime::detail::validate_config(config);
}

/** @brief 生成当前线程模式的默认选项。 */
constexpr runtime_options current_thread_defaults() noexcept {
  return runtime_options{.mode = faio::runtime::mode::current_thread};
}

/** @brief 生成多工作线程模式的默认选项。 */
constexpr runtime_options multi_thread_defaults() noexcept {
  return {};
}
}  // namespace faio::experimental
#endif
