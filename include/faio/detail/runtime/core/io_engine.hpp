#ifndef FAIO_DETAIL_RUNTIME_CORE_IO_ENGINE_HPP
#define FAIO_DETAIL_RUNTIME_CORE_IO_ENGINE_HPP

#include "faio/detail/coroutine/scheduler.hpp"
#include "faio/detail/io/uring/io_completion.hpp"
#include "faio/detail/io/uring/io_uring.hpp"
#include "faio/detail/io/uring/waker.hpp"
#include "faio/detail/runtime/core/config.hpp"
#include "faio/detail/runtime/core/timer/timer.hpp"
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
namespace faio::runtime::detail {

class io_engine;
// I/O 资源定位入口，与通用调度线程绑定分开；协程模块不依赖此类型。
inline thread_local io_engine *current_io_engine{nullptr};

// io_engine 类，用于IO处理
class io_engine {
public:
  // stop_callback 只入队，不跨线程操作 liburing 的 SQ；共享状态保证
  // 原始 CQE 先完成时不会再访问已释放的 awaiter 帧。
  void request_cancel(std::shared_ptr<io::detail::io_cancel_state> state) {
    {
      std::lock_guard lock(_cancel_mutex);
      _cancel_queue.push_back(std::move(state));
    }
    _has_cancel_requests.store(true, std::memory_order_release);
    wake_up();
  }
  io_engine(const runtime_config &config) : _uring(config) { current_io_engine = this; }
  ~io_engine() { current_io_engine = nullptr; }

public:
  // 等待定时器到期并驱动IO处理
  // 注意这里使用了C++23的deducing this语法，可以自动推导出this指针的类型
  template <ready_sink sink_type>
  void wait_and_drive(this io_engine &engine, sink_type &sink) {
    // 等待定时器到期
    engine._uring.wait(engine._timer.next_deadline_ms());
    // 处理已经完成的IO
    engine.drive(sink);
  }

  // 驱动函数 ,用于处理已经完成的IO
  template <ready_sink sink_type>
  bool drive(this io_engine &engine, sink_type &sink) {
    // 定义编译期常量
    constexpr const std::size_t SIZE = LOCAL_QUEUE_CAPACITY;
    std::array<io::detail::io_completion_t, SIZE> completions;

    // 预读完成队列
    auto completed_count = engine._uring.peek_batch(completions);
    // 遍历处理
    // 逻辑：如果定时器任务不为空，则移除定时器任务，并设置结果
    // 如果定时器任务为空，则设置结果，并推送到本地队列
    for (std::size_t i = 0; i < completed_count; i++) {
      auto user_data = completions[i].data();
      // 跳过 waker 的 eventfd 完成事件 (其 user_data 为 nullptr)
      if (user_data == nullptr) {
        continue;
      }
      if (user_data->timer_task != nullptr) {
        engine._timer.remove_task(user_data->timer_task);
      }
      user_data->result = completions[i].expected();
      if (user_data->cancel_state)
        user_data->cancel_state->done.store(true, std::memory_order_release);
      // 只报告就绪，队列布局和溢出由具体 sink 决定；模板调用可内联。
      sink.enqueue_ready(user_data->handle);
    }
    // 消费完成队列
    engine._uring.consume(completed_count);
    // 在所属 worker 上提交 cancel SQE；取消 CQE 使用空 user_data，
    // 因此只有原操作的 CQE 能恢复等待协程。
    std::vector<std::shared_ptr<io::detail::io_cancel_state>> cancels;
    if (engine._has_cancel_requests.exchange(false, std::memory_order_acq_rel)) {
      std::lock_guard lock(engine._cancel_mutex);
      cancels.swap(engine._cancel_queue);
    }
    bool retry_cancel = false;
    for (auto& state : cancels) {
      if (state->done.load(std::memory_order_acquire)) continue;
      auto* sqe = engine._uring.get_sqe();
      if (!sqe) {
        engine._uring.reset_and_submit();
        sqe = engine._uring.get_sqe();
      }
      if (!sqe) {
        std::lock_guard lock(engine._cancel_mutex);
        engine._cancel_queue.push_back(std::move(state));
        engine._has_cancel_requests.store(true, std::memory_order_release);
        retry_cancel = true;
        continue;
      }
      io_uring_prep_cancel(sqe, state->target, 0);
      io_uring_sqe_set_data(sqe, nullptr);
    }
    // 处理定时器任务
    auto timer_count = engine._timer.poll(sink);
    // 更新完成队列数量
    completed_count += timer_count;
    // 开始监视唤醒
    engine._waker.start_watch();
    // 重置提交计数并提交
    engine._uring.reset_and_submit();
    return completed_count > 0 || retry_cancel;
  }

  // 唤醒IO处理引擎
  void wake_up(this io_engine &engine) noexcept { engine._waker.wake_up(); }

private:
  io::detail::IOuring _uring; // uring实例
  io::detail::Waker _waker;   // 唤醒器
  timer::Timer _timer;        // 定时器
  std::mutex _cancel_mutex; // 保护其他线程提交的取消请求，SQ 仍仅由所属线程操作。
  std::atomic<bool> _has_cancel_requests{false}; // 无取消请求时跳过互斥锁。
  std::vector<std::shared_ptr<io::detail::io_cancel_state>> _cancel_queue; // 保证取消状态活到所属线程消费。
};
} // namespace faio::runtime::detail
#endif // FAIO_DETAIL_RUNTIME_CORE_IO_ENGINE_HPP
