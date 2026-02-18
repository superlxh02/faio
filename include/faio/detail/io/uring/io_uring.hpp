#ifndef FAIO_DETAIL_IO_URING_IO_URING_HPP
#define FAIO_DETAIL_IO_URING_IO_URING_HPP

#include "faio/detail/io/uring/io_completion.hpp"
#include "faio/detail/runtime/core/config.hpp"
#include "fastlog/fastlog.hpp"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <format>
#include <iterator>
#include <liburing.h>
namespace faio::io::detail {

class IOuring;

// 线程局部存储，当前uring实例指针
inline thread_local IOuring *current_uring{nullptr};

// 封装uring实例，提供uring操作接口
class IOuring {
public:
  IOuring(const runtime::detail::Config &config)
      : _submit_interval(config._submit_interval) {
    io_uring_queue_init(config._num_events, &_uring, 0);
    assert(current_uring == nullptr);
    current_uring = this;
  }

  ~IOuring() {
    io_uring_queue_exit(&_uring);
    current_uring = nullptr;
  }

public:
  /// 获取uring实例
  [[nodiscard]] struct io_uring *uring() noexcept { return &_uring; }

  /// 获取sqe
  [[nodiscard]] io_uring_sqe *get_sqe() noexcept {
    return io_uring_get_sqe(uring());
  }

  /// 预读完成队列
  [[nodiscard]] std::size_t peek_batch(std::span<io_completion_t> expected) {
    return io_uring_peek_batch_cqe(
        &_uring, reinterpret_cast<io_uring_cqe **>(expected.data()),
        expected.size());
  }
  // 消费完成队列
  void consume(std::size_t count) { io_uring_cq_advance(&_uring, count); }

  // 标记完成队列已处理
  void seen(io_uring_cqe *cqe) { io_uring_cqe_seen(&_uring, cqe); }

  // 提交
  void submit() {
    _submit_tick += 1;
    if (_submit_tick == _submit_interval) {
      reset_and_submit();
    }
  }

  /// 等待完成队列,可指定超时时间
  void wait(std::optional<time_t> timeout) {
    io_uring_cqe *cqe{nullptr};

    if (timeout) {
      // 构造超时时间结构体
      struct __kernel_timespec ts{
          .tv_sec = timeout.value() / 1000,
          .tv_nsec = (timeout.value() % 1000) * 1000000,
      };
      if (auto res = io_uring_wait_cqe_timeout(&_uring, &cqe, &ts); res < 0) {
        // -ETIME 是正常超时，不是错误
        if (res != -ETIME) {
          fastlog::console.error("wait cqe failed, {}", strerror(-res));
        }
      }
    } else {
      if (auto res = io_uring_wait_cqe(&_uring, &cqe); res < 0) {
        fastlog::console.error("wait cqe failed, {}", strerror(-res));
      }
    }
  }
  // 重置提交计数并提交
  void reset_and_submit() {
    _submit_tick = 0;
    if (auto ret = io_uring_submit(&_uring); ret < 0) {
      fastlog::console.error("submit sqes failed, {}", strerror(-ret));
    }
  }

private:
  io_uring _uring;                // uring实例
  std::uint32_t _submit_interval; // 提交间隔
  std::uint32_t _submit_tick{0};  // 提交计数
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_URING_IO_URING_HPP