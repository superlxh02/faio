/**
 * @file faio/detail/io/testing/uring_control_policy.hpp
 * @brief 仅专用测试启用的原生控制脚本。
 */
#pragma once
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
#include "faio/detail/io/testing/control_ref.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <liburing.h>
#include <semaphore>
#include <signal.h>
#include <unistd.h>

namespace faio::io::detail {
enum class uring_control_test_point {
  POLL_AFTER_LOAD,
  POLL_AFTER_CONSUME,
  POLL_BEFORE_WAIT,
  WAKE_AFTER_EXCHANGE,
  WAKE_BEFORE_WRITE,
  HARVEST_BEFORE_DRAIN,
  HARVEST_AFTER_DRAIN
};

/** @brief io_uring 控制唤醒仲裁状态。 */
struct uring_control_gate {
  std::binary_semaphore arrived{0}, released{0};
  std::atomic_bool expired{false};

  /** @brief 通知测试线程已到达暂停点，并等待放行。 */
  void stop() noexcept {
    arrived.release();
    if (!released.try_acquire_for(std::chrono::seconds(1)))
      expired.store(true, std::memory_order_relaxed);
  }

  /** @brief 等待被测线程到达暂停点。 */
  bool await() noexcept { return arrived.try_acquire_for(std::chrono::seconds(1)); }

  /** @brief 放行暂停的被测线程。 */
  void open() noexcept { released.release(); }
};

/** @brief 所有场景共享同一具体数据布局；调用目标不随脚本配置改变。 */
struct uring_control_policy {
  bool pressure_enabled{}, wake_enabled{};
  unsigned submits{}, fail_at{1};
  int error{EAGAIN};
  io_uring* ring{}; /**< 借用的原生队列，由被测 backend 保证在脚本调用期间有效。 */
  uring_control_gate gate;
  uring_control_test_point pause_on{uring_control_test_point::POLL_AFTER_LOAD};
  bool pause_enabled{}, interrupt_first_write{};
  std::atomic<unsigned> hits{0}, writes{0}, successful_writes{0}, waits{0}, drains{0};
  std::atomic<int> last_write_error{0};
  std::atomic_bool invalid_wait{false};

  /** @brief 提交原生队列请求，必要时按脚本注入背压。 */
  int submit(io_uring* queue) noexcept {
    if (pressure_enabled && ++submits == fail_at)
      return -error;
    return ::io_uring_submit(queue);
  }

  /** @brief 记录控制通知测试点，并按脚本在指定位置暂停。 */
  void step(uring_control_test_point point) noexcept {
    if (!wake_enabled)
      return;
    if (point == uring_control_test_point::HARVEST_AFTER_DRAIN)
      drains.fetch_add(1, std::memory_order_relaxed);
    if (pause_enabled && point == pause_on && hits.fetch_add(1, std::memory_order_relaxed) == 0)
      gate.stop();
  }

  /** @brief 写入控制通知，并按脚本记录结果或注入中断。 */
  ssize_t write(int fd, const void* value, std::size_t size) noexcept {
    if (!wake_enabled)
      return ::write(fd, value, size);
    const auto attempt = writes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (interrupt_first_write && attempt == 1) {
      errno = EINTR;
      last_write_error.store(EINTR, std::memory_order_relaxed);
      return -1;
    }
    const auto result = ::write(fd, value, size);
    if (result >= 0)
      successful_writes.fetch_add(1, std::memory_order_relaxed);
    else
      last_write_error.store(errno, std::memory_order_relaxed);
    return result;
  }

  /** @brief 等待原生队列事件，并按脚本记录结果或注入背压。 */
  int wait() noexcept {
    waits.fetch_add(1, std::memory_order_relaxed);
    if (pressure_enabled)
      return -EAGAIN;
    __kernel_timespec duration{0, 500000000};
    io_uring_getevents_arg argument{};
    argument.ts = reinterpret_cast<std::uintptr_t>(&duration);
    const int result = ::io_uring_enter2(ring->ring_fd,
                                         0,
                                         1,
                                         IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
                                         reinterpret_cast<sigset_t*>(&argument),
                                         sizeof(argument));
    if (result < 0 && result != -EINTR)
      invalid_wait.store(true, std::memory_order_relaxed);
    return result;
  }
};
}  // namespace faio::io::detail
#endif
