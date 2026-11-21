/**
 * @file faio/detail/runtime/config_builder.hpp
 * @brief 运行时配置值与构建器，不引入调度、IO 域或默认运行时实现。
 */
#ifndef FAIO_DETAIL_RUNTIME_CONFIG_BUILDER_HPP
#define FAIO_DETAIL_RUNTIME_CONFIG_BUILDER_HPP

#include "faio/detail/runtime/common/config.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace faio {
/** @brief 按值构建配置；修改构建器不会影响已经启动的运行时。 */
class config_builder {
 public:
  /** @brief 设置运行时的单线程或多线程驱动模式。 */
  config_builder& set_mode(runtime::mode selected) {
    _config._mode = selected;
    return *this;
  }

#if defined(__linux__)
  /**  @brief 验证构建时固定的 Linux 后端；不匹配在运行时初始化前拒绝。 */
  config_builder& set_io_backend(runtime::io_backend selected) noexcept {
    _config._requested_io_backend = selected;
    return *this;
  }
#endif
  /**  @brief 设置独立文件服务容量；与协程 worker 和用户阻塞池隔离。 */
  config_builder& set_filesystem_threads(std::size_t count) noexcept {
    _config._filesystem_threads = count;
    return *this;
  }

  /** @brief 设置独立名称解析服务的线程上限。 */
  config_builder& set_resolver_threads(std::size_t count) noexcept {
    _config._resolver_threads = count;
    return *this;
  }

  /** @brief 设置文件服务中等待执行的请求容量。 */
  config_builder& set_filesystem_queue_limit(std::size_t count) noexcept {
    _config._filesystem_queue_limit = count;
    return *this;
  }

  /** @brief 设置名称解析服务中等待执行的请求容量。 */
  config_builder& set_resolver_queue_limit(std::size_t count) noexcept {
    _config._resolver_queue_limit = count;
    return *this;
  }

  /** @brief 设置用户阻塞池中等待执行的任务容量。 */
  config_builder& set_blocking_queue_limit(std::size_t count) noexcept {
    _config._blocking_queue_limit = count;
    return *this;
  }

  /** @brief 构造采用默认值的运行时配置。 */
  config_builder() = default;

  /** @brief 释放构建器保存的配置值。 */
  ~config_builder() = default;

 public:
  /** @brief 设置单次 reactor 事件批量大小，实际限制为 1 至 256；不限制全局活跃请求数。 */
  config_builder& set_num_events(std::size_t num_events) {
    _config._num_events = num_events;
    return *this;
  }

  /** @brief 设置提交批量间隔参数；epoll/kqueue 在等待时直接尝试系统调用。 */
  config_builder& set_submit_interval(uint32_t submit_interval) {
    _config._submit_interval = submit_interval;
    return *this;
  }

  /** @brief 设置工作线程数，同时决定共享调度域注册容量和退出屏障容量。 */
  config_builder& set_num_workers(std::size_t num_workers) {
    _config._num_workers = num_workers;
    return *this;
  }

  /** @brief 显式指定工作线程的 CPU；空列表沿用调用方或系统的调度，current_thread 仅接受一个 CPU。 */
  config_builder& set_worker_cpus(std::vector<int> cpus) noexcept {
    _config._worker_cpus = std::move(cpus);
    return *this;
  }

  /** @brief 设置每个工作线程的不可窃取队列固定容量；默认 256，运行期间不扩容。 */
  config_builder& set_no_stealing_queue_capacity(std::size_t capacity) noexcept {
    _config._no_stealing_queue_capacity = capacity;
    return *this;
  }

  /** @brief 设置运行时大帧空闲池的字节额度；默认值 0 表示关闭，不限制活动帧总量。 */
  config_builder& set_large_frame_pool_limit_bytes(std::size_t bytes) noexcept {
    _config._large_frame_pool_limit_bytes = bytes;
    return *this;
  }

  /** @brief 设置用户阻塞池的工作线程上限。 */
  config_builder& set_max_blocking_threads(std::size_t count) {
    _config._max_blocking_threads = count;
    return *this;
  }

  /** @brief 选择用户阻塞池的线程生命周期策略。默认按需启动和空闲回收，可选择固定预热。 */
  config_builder& set_blocking_pool_policy(runtime::blocking_pool_policy selected) noexcept {
    _config._blocking_pool_policy = selected;
    return *this;
  }

  /** @brief 设置弹性用户阻塞池的线程空闲保活时长。默认 10 秒。 */
  config_builder& set_blocking_keep_alive(std::chrono::milliseconds duration) {
    _config._blocking_keep_alive = duration;
    return *this;
  }

  /** @brief 设置持续执行协程时轮询 I/O 的事件循环间隔。 */
  config_builder& set_io_interval(uint32_t io_interval) {
    _config._io_interval = io_interval;
    return *this;
  }

  /** @brief 设置本地工作持续存在时检查全局队列的公平性间隔。 */
  config_builder& set_global_queue_interval(uint32_t global_queue_interval) {
    _config._global_queue_interval = global_queue_interval;
    return *this;
  }

  /** @brief 设置入睡前的有限轮询次数；零适合希望更快休眠的场景。 */
  config_builder& set_idle_spin_count(uint32_t idle_spin_count) {
    _config._idle_spin_count = idle_spin_count;
    return *this;
  }

  /** @brief 返回配置副本；运行时构造/配置入口负责最终验证。 */
  runtime::config build() const { return _config; }

 private:
  runtime::config _config; /**< 构建中的配置副本，不修改已经启动的运行时。 */
};

/** @brief 运行时配置构建器的兼容名称。 */
using ConfigBuilder = config_builder;
}  // namespace faio
#endif
