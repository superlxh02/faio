#ifndef FAIO_FAIO_HPP
#define FAIO_FAIO_HPP
#if defined(_WIN32)
#include "faio/detail/io/platform/windows_framework.hpp"
#else
#include "faio/detail/coroutine.hpp"
#include "faio/detail/fs.hpp"
#include "faio/detail/io.hpp"
#include "faio/detail/io/platform/async_fd.hpp"
#include "faio/detail/io/util/adapters.hpp"
#include "faio/detail/io/util/buffered.hpp"
#include "faio/detail/io/util/endian.hpp"
#include "faio/detail/io/util/memory_stream.hpp"
#include "faio/detail/net.hpp"
#include "faio/detail/runtime/context.hpp"
#include "faio/detail/runtime/default.hpp"
#include "faio/detail/sync.hpp"
#include "faio/detail/time.hpp"
#include "faio/log.hpp"
#include <chrono>

namespace faio {

// 默认运行时入口：worker 内直接使用 TLS 调度器；任意外部线程通过
// 进程默认运行时提交，不要求调用线程曾经创建过运行时。
template <class T> join_handle<T> spawn(task<T> child) {
  if (auto scheduler = detail::current_scheduler())
    return join_handle<T>{detail::start_observed(scheduler, std::move(child),
                                                 detail::current_tracker,
                                                 detail::current_stop_token)};
  return runtime::detail::default_service().with_context(
      [&](runtime::detail::runtime_context &ctx) -> join_handle<T> {
        return join_handle<T>{detail::start_observed(ctx.scheduler(),
                                                     std::move(child), nullptr,
                                                     {}, ctx.task_lifetime())};
      });
}

template <class T> void spawn_detached(task<T> child) {
  if (auto scheduler = detail::current_scheduler()) {
    detail::start_unobserved(scheduler, std::move(child),
                             detail::current_tracker);
    return;
  }
  runtime::detail::default_service().with_context(
      [&](runtime::detail::runtime_context &ctx) {
        detail::start_unobserved(ctx.scheduler(), std::move(child), nullptr,
                                 ctx.task_lifetime());
      });
}

/** @brief 将同步函数提交到预热的有界用户阻塞池；等待只挂起协程，不阻塞IO
 * worker。 */
template <class F> auto spawn_blocking(F &&function) {
  if (auto *pool = detail::current_blocking_pool())
    return runtime::detail::start_blocking(
        *pool, std::forward<F>(function), detail::current_tracker,
        detail::current_stop_token, detail::current_task_lifetime());
  return runtime::detail::default_service().with_context(
      [&](runtime::detail::runtime_context &ctx) {
        return ctx.submit_blocking(std::forward<F>(function));
      });
}

template <typename T> inline auto block_on(task<T> t) -> T {
  if (detail::on_runtime_worker())
    throw std::logic_error("不能在 worker 线程调用 block_on；请 co_await task");
  return runtime::detail::default_service().with_context(
      [&](runtime::detail::runtime_context &ctx) -> T {
        return ctx.block_on(std::move(t));
      });
}

// 按值构建配置；修改构建器不会影响已经启动的运行时。
class config_builder {
public:
  config_builder &set_mode(runtime::mode selected) {
    _config._mode = selected;
    return *this;
  }

#if defined(__linux__)
  /** @brief 选择已编译的 Linux 后端，运行时初始化前检查可用性。 */
  config_builder &set_io_backend(runtime::io_backend selected) noexcept {
    _config._requested_io_backend = selected;
    return *this;
  }
#endif
  /** @brief 设置独立文件服务容量；与协程 worker 和用户阻塞池隔离。 */
  config_builder &set_filesystem_threads(std::size_t count) noexcept {
    _config._filesystem_threads = count;
    return *this;
  }
  config_builder &set_resolver_threads(std::size_t count) noexcept {
    _config._resolver_threads = count;
    return *this;
  }
  config_builder &set_filesystem_queue_limit(std::size_t count) noexcept {
    _config._filesystem_queue_limit = count;
    return *this;
  }
  config_builder &set_resolver_queue_limit(std::size_t count) noexcept {
    _config._resolver_queue_limit = count;
    return *this;
  }
  config_builder &set_blocking_queue_limit(std::size_t count) noexcept {
    _config._blocking_queue_limit = count;
    return *this;
  }
  config_builder() = default;
  ~config_builder() = default;

public:
  // 设置单次reactor事件批量大小，实际批量限制为1至256；不限制全局活跃请求数。
  config_builder &set_num_events(std::size_t num_events) {
    _config._num_events = num_events;
    return *this;
  }

  // 保留提交批量间隔配置；当前epoll/kqueue请求在await时直接尝试系统调用。
  config_builder &set_submit_interval(uint32_t submit_interval) {
    _config._submit_interval = submit_interval;
    return *this;
  }

  // 设置工作线程数，同时决定共享调度域注册容量和退出屏障容量。
  config_builder &set_num_workers(std::size_t num_workers) {
    _config._num_workers = num_workers;
    return *this;
  }

  config_builder &set_max_blocking_threads(std::size_t count) {
    _config._max_blocking_threads = count;
    return *this;
  }

  config_builder &set_blocking_keep_alive(std::chrono::milliseconds duration) {
    _config._blocking_keep_alive = duration;
    return *this;
  }

  // 设置持续执行协程时轮询 I/O 的事件循环间隔。
  config_builder &set_io_interval(uint32_t io_interval) {
    _config._io_interval = io_interval;
    return *this;
  }

  // 设置本地工作持续存在时检查全局队列的公平性间隔。
  config_builder &set_global_queue_interval(uint32_t global_queue_interval) {
    _config._global_queue_interval = global_queue_interval;
    return *this;
  }

  // 设置入睡前的有限轮询次数；零适合希望更快休眠的场景。
  config_builder &set_idle_spin_count(uint32_t idle_spin_count) {
    _config._idle_spin_count = idle_spin_count;
    return *this;
  }

  // 返回配置副本；运行时构造/配置入口负责最终验证。
  runtime::config build() const noexcept { return _config; }

private:
  runtime::config _config; // 构建中的配置副本，不修改已经启动的运行时。
};
// 保留已有公开名称的兼容别名，实现类型统一采用下划线命名。
using ConfigBuilder = config_builder;
} // namespace faio

#endif // Windows框架/完整POSIX实现
#endif // FAIO_FAIO_HPP
