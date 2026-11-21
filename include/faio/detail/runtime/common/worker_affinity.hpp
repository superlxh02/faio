/**
 * @file faio/detail/runtime/common/worker_affinity.hpp
 * @brief 显式 worker CPU 配置与线程范围内的亲和性守卫。
 */
#ifndef FAIO_DETAIL_RUNTIME_COMMON_WORKER_AFFINITY_HPP
#define FAIO_DETAIL_RUNTIME_COMMON_WORKER_AFFINITY_HPP

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#if defined(__linux__)
#include <sched.h>
#endif
#if defined(FAIO_TEST_WORKER_AFFINITY)
#include <atomic>
#endif

namespace faio::runtime::detail {
#if defined(FAIO_TEST_WORKER_AFFINITY)
/** @brief 仅独立测试目标启用；生产构建没有状态、计数或注入分支。 */
inline std::atomic<std::ptrdiff_t> worker_affinity_test_fail_worker{-1};
inline std::atomic<std::ptrdiff_t> worker_affinity_test_fail_launch_after{-1};
inline std::atomic<std::size_t> worker_affinity_test_syscalls{};
inline std::atomic<std::size_t> worker_affinity_test_live_scopes{};
#endif

#if defined(__linux__)
/** @brief 读取线程的 CPU 亲和性设置。 */
inline int read_thread_affinity(cpu_set_t& mask) noexcept {
#if defined(FAIO_TEST_WORKER_AFFINITY)
  worker_affinity_test_syscalls.fetch_add(1, std::memory_order_relaxed);
#endif
  return ::sched_getaffinity(0, sizeof(mask), &mask);
}

/** @brief 设置线程的 CPU 亲和性。 */
inline int write_thread_affinity(const cpu_set_t& mask) noexcept {
#if defined(FAIO_TEST_WORKER_AFFINITY)
  worker_affinity_test_syscalls.fetch_add(1, std::memory_order_relaxed);
#endif
  return ::sched_setaffinity(0, sizeof(mask), &mask);
}

/** @brief 读取当前线程亲和性设置中的 CPU 编号。 */
inline int current_affinity_cpu() noexcept {
#if defined(FAIO_TEST_WORKER_AFFINITY)
  worker_affinity_test_syscalls.fetch_add(1, std::memory_order_relaxed);
#endif
  return ::sched_getcpu();
}
#endif

/**
 *  @brief 启动时验证 CPU 编号与工作线程一一对应、没有重复、可以表示，且属于调用线程当前允许使用的集合。
 * @details Linux 允许的 CPU 掩码由内核提供，排除当前不可用或离线的 CPU；之后的热拔插或
 * cpuset 外部修改仍可能使实际绑定失败，由守卫保留其系统错误。
 */
inline void validate_worker_affinity(std::span<const int> cpus, std::size_t workers) {
  if (cpus.empty())
    return;
  if (cpus.size() != workers)
    throw std::invalid_argument("worker CPU list must contain exactly one CPU per worker");
  for (std::size_t index = 0; index < cpus.size(); ++index) {
    if (cpus[index] < 0)
      throw std::invalid_argument("worker CPU ID must be nonnegative");
    if (std::find(cpus.begin(), cpus.begin() + static_cast<std::ptrdiff_t>(index), cpus[index])
        != cpus.begin() + static_cast<std::ptrdiff_t>(index))
      throw std::invalid_argument("worker CPU list must not contain duplicates");
  }
#if defined(__linux__)
  for (const auto cpu : cpus)
    if (cpu >= CPU_SETSIZE)
      throw std::invalid_argument("worker CPU ID exceeds the supported cpu_set_t range");
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (read_thread_affinity(allowed) != 0)
    throw std::system_error(errno, std::generic_category(), "read allowed worker CPU mask");
  for (const auto cpu : cpus)
    if (!CPU_ISSET(cpu, &allowed))
      throw std::invalid_argument("worker CPU is offline or outside the caller's allowed CPU mask");
#else
  throw std::system_error(std::make_error_code(std::errc::not_supported),
                          "explicit worker CPU affinity is not supported on this platform");
#endif
}

/**
 *  @brief 仅在显式配置时改变当前线程的 CPU 掩码，析构完成前恢复进入时的掩码。
 * @details current_thread 驱动器应在独占 drive 锁内创建守卫，使其覆盖执行和清理过程。
 * 恢复失败或掩码与原值不一致时，输出最少必要的诊断后终止，避免在线程状态未还原时继续执行。
 */
class worker_affinity_scope {
 public:
  /** @brief 构造工作线程 CPU 亲和性作用域。 */
  explicit worker_affinity_scope(std::span<const int> cpus, std::size_t worker = 0) {
    if (cpus.empty())
      return;
    if (worker >= cpus.size())
      throw std::invalid_argument("worker CPU index is outside the explicit CPU list");
#if defined(__linux__)
    const auto cpu = cpus[worker];
    if (cpu < 0 || cpu >= CPU_SETSIZE)
      throw std::invalid_argument("worker CPU ID exceeds the supported cpu_set_t range");
    if (read_thread_affinity(previous_) != 0)
      throw std::system_error(errno, std::generic_category(), "read previous worker CPU mask");
    if (!CPU_ISSET(cpu, &previous_))
      throw std::invalid_argument("worker CPU is offline or outside the driver's allowed CPU mask");
    cpu_set_t selected;
    CPU_ZERO(&selected);
    CPU_SET(cpu, &selected);
    if (write_thread_affinity(selected) != 0) {
      const auto error = errno;
      throw std::system_error(
          error, std::generic_category(), "bind worker CPU " + std::to_string(cpu));
    }
    active_ = true;
    try {
      cpu_set_t actual;
      CPU_ZERO(&actual);
      if (read_thread_affinity(actual) != 0)
        throw std::system_error(errno, std::generic_category(), "verify worker CPU mask");
      if (CPU_COUNT(&actual) != 1 || !CPU_ISSET(cpu, &actual))
        throw std::runtime_error("worker CPU mask differs from the requested single CPU");
      const auto actual_cpu = current_affinity_cpu();
      if (actual_cpu < 0)
        throw std::system_error(errno, std::generic_category(), "verify current worker CPU");
      if (actual_cpu != cpu)
        throw std::runtime_error("worker is not executing on the requested CPU");
    } catch (...) {
      restore();
      throw;
    }
#if defined(FAIO_TEST_WORKER_AFFINITY)
    worker_affinity_test_live_scopes.fetch_add(1, std::memory_order_relaxed);
    counted_ = true;
#endif
#else
    throw std::system_error(std::make_error_code(std::errc::not_supported),
                            "explicit worker CPU affinity is not supported on this platform");
#endif
  }

  /** @brief 禁止复制构造工作线程 CPU 亲和性作用域。 */
  worker_affinity_scope(const worker_affinity_scope&) = delete;
  /** @brief 禁止复制赋值。 */
  worker_affinity_scope& operator=(const worker_affinity_scope&) = delete;

  /** @brief 退出作用域时恢复原 CPU 亲和性设置。 */
  ~worker_affinity_scope() noexcept {
#if defined(__linux__)
    restore();
#if defined(FAIO_TEST_WORKER_AFFINITY)
    if (counted_)
      worker_affinity_test_live_scopes.fetch_sub(1, std::memory_order_relaxed);
#endif
#endif
  }

 private:
#if defined(__linux__)
  /** @brief 恢复进入作用域前的 CPU 亲和性设置。 */
  void restore() noexcept {
    if (!active_)
      return;
    cpu_set_t restored;
    CPU_ZERO(&restored);
    if (write_thread_affinity(previous_) != 0 || read_thread_affinity(restored) != 0
        || !CPU_EQUAL(&restored, &previous_)) {
      std::fputs("faio: failed to restore caller CPU affinity\n", stderr);
      std::terminate();
    }
    active_ = false;
  }

  cpu_set_t previous_{};
  bool active_{};
#if defined(FAIO_TEST_WORKER_AFFINITY)
  bool counted_{};
#endif
#endif
};
}  // namespace faio::runtime::detail
#endif
