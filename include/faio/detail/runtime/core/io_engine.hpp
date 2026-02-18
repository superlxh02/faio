#ifndef FAIO_DETAIL_RUNTIME_CORE_ENGINE_HPP
#define FAIO_DETAIL_RUNTIME_CORE_ENGINE_HPP

#include "faio/detail/io/uring/io_completion.hpp"
#include "faio/detail/io/uring/io_uring.hpp"
#include "faio/detail/io/uring/waker.hpp"
#include "faio/detail/runtime/core/config.hpp"
#include "faio/detail/runtime/core/timer/timer.hpp"
#include <array>
namespace faio::runtime::detail {

class IOEngine;
/// 线程局部存储，当前io引擎实例指针,每个线程只能有一个io引擎实例
inline thread_local IOEngine *current_io_engine{nullptr};

// IOEngine 类，用于IO处理
class IOEngine {
public:
  IOEngine(const Config &config) : _uring(config) { current_io_engine = this; }
  ~IOEngine() { current_io_engine = nullptr; }

public:
  // 等待定时器到期并驱动IO处理
  // 注意这里使用了C++23的deducing this语法，可以自动推导出this指针的类型
  template <typename LocalQueue, typename GlobalQueue>
  void wait_and_drive(this IOEngine &engine, LocalQueue &local_queue,
                      GlobalQueue &global_queue) {
    // 等待定时器到期
    engine._uring.wait(engine._timer.next_deadline_ms());
    // 处理已经完成的IO
    engine.drive(local_queue, global_queue);
  }

  // 驱动函数 ,用于处理已经完成的IO
  template <typename LocalQueue, typename GlobalQueue>
  bool drive(this IOEngine &engine, LocalQueue &local_queue,
             GlobalQueue &global_queue) {
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
      local_queue.push_back(user_data->handle, global_queue);
    }
    // 消费完成队列
    engine._uring.consume(completed_count);
    // 处理定时器任务
    auto timer_count = engine._timer.poll(local_queue, global_queue);
    // 更新完成队列数量
    completed_count += timer_count;
    // 开始监视唤醒
    engine._waker.start_watch();
    // 重置提交计数并提交
    engine._uring.reset_and_submit();
    return completed_count > 0;
  }

  // 唤醒IO处理引擎
  void wake_up(this IOEngine &engine) { engine._waker.wake_up(); }

private:
  io::detail::IOuring _uring; // uring实例
  io::detail::Waker _waker;   // 唤醒器
  timer::Timer _timer;        // 定时器
};
} // namespace faio::runtime::detail
#endif // FAIO_DETAIL_RUNTIME_CORE_ENGINE_HPP