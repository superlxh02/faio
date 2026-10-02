#ifndef FAIO_FAIO_HPP
#define FAIO_FAIO_HPP
#include "faio/log.hpp"
#include "faio/detail/coroutine.hpp"
#include "faio/detail/io.hpp"
#include "faio/detail/net.hpp"
#include "faio/detail/runtime/context.hpp"
#include "faio/detail/runtime/default.hpp"
#include "faio/detail/sync.hpp"
#include "faio/detail/time.hpp"

namespace faio {

using runtime_context = runtime::detail::runtime_context;

// 默认运行时入口：worker 内直接使用 TLS 调度器；任意外部线程通过
// 进程默认运行时提交，不要求调用线程曾经创建过 runtime_context。
template <class T> join_handle<T> spawn(task<T> child) {
  if (auto scheduler = detail::current_scheduler())
    return join_handle<T>{detail::start_observed(
        scheduler, std::move(child), detail::current_tracker,
        detail::current_stop_token)};
  return runtime::detail::default_service().with_context(
      [&](runtime_context& ctx) -> join_handle<T> {
        return join_handle<T>{detail::start_observed(
            ctx.scheduler(), std::move(child), nullptr, {}, ctx.task_lifetime())};
      });
}

template <class T> void spawn_detached(task<T> child) {
  if (auto scheduler = detail::current_scheduler()) {
    detail::start_unobserved(scheduler, std::move(child), detail::current_tracker);
    return;
  }
  runtime::detail::default_service().with_context([&](runtime_context& ctx) {
    detail::start_unobserved(ctx.scheduler(),
                             std::move(child), nullptr, ctx.task_lifetime());
  });
}

// 旧的显式运行时入口保留给需要隔离多个运行时的调用方。
template <class T> join_handle<T> spawn(runtime_context& ctx, task<T> child) {
  return join_handle<T>{detail::start_observed(
      ctx.scheduler(), std::move(child), nullptr, {}, ctx.task_lifetime())};
}
template <class T> void spawn_detached(runtime_context& ctx, task<T> child) {
  detail::start_unobserved(ctx.scheduler(),
                           std::move(child), nullptr, ctx.task_lifetime());
}

template <typename T>
inline auto block_on(task<T> t) -> T {
  if (detail::on_runtime_worker())
    throw std::logic_error("不能在 worker 线程调用 block_on；请 co_await task");
  return runtime::detail::default_service().with_context(
      [&](runtime_context& ctx) -> T { return ctx.block_on(std::move(t)); });
}

// block_on: 阻塞执行协程
template <typename T>
inline auto block_on(runtime_context &ctx, task<T> t) -> T {
  return ctx.block_on(std::move(t));
}

// wait_all: 并行执行多个协程
template <typename... Ts>
inline auto wait_all(runtime_context &ctx, task<Ts>... tasks)
    -> std::tuple<Ts...> {
  return ctx.wait_all(std::move(tasks)...);
}

// 按值构建配置；修改构建器不会影响已经启动的运行时。
class config_builder {
public:
  config_builder() = default;
  ~config_builder() = default;

public:
  // 设置 io_uring 队列容量，在启动运行时之前验证非零。
  config_builder &set_num_events(std::size_t num_events) {
    _config._num_events = num_events;
    return *this;
  }

  // 设置 I/O 操作自动批量提交间隔，事件循环仍会刷新待提交请求。
  config_builder &set_submit_interval(uint32_t submit_interval) {
    _config._submit_interval = submit_interval;
    return *this;
  }

  // 设置工作线程数，同时决定共享调度域注册容量和退出屏障容量。
  config_builder &set_num_workers(std::size_t num_workers) {
    _config._num_workers = num_workers;
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

#endif // FAIO_FAIO_HPP
