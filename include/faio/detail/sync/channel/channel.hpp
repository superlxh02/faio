#ifndef FAIO_DETAIL_SYNC__CHANNEL_CHANNEL_HPP
#define FAIO_DETAIL_SYNC__CHANNEL_CHANNEL_HPP

#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/runtime/core/poller.hpp"
#include "faio/detail/sync/channel/ring_buffer.hpp"
#include "faio/detail/sync/mutex.hpp"
namespace faio::sync::detail {

// Channel类：实现有界多生产者多消费者通道，内部使用环形缓冲区实现
// Channel类通信的本质 ：
// 1.sender和receiver通过缓冲区和recviver的_result进行通信

// sender和receiver的两种时序 不同的通信过程：
/*
1.sender先被调度
三种发送数据的操作：
  //（1）将当前值放入对应接收者_result里，恢复receiver拿数据 ->
条件是已经有等待接受数据的receiver
  //（2）放入缓冲区 -> 条件是缓冲区没满
  // (3）什么都不做,只是把自己放入等待发送队列 -> 条件是缓冲区满了且通道未关闭

2.receiver先被调度
  //（1）将当前值从sender的_value中取走，恢复sender拿数据 ->
条件是已经有等待发送数据的sender
  //（2）从缓冲区中取出值 -> 条件是缓冲区不为空
  // (3）什么都不做,只是把自己放入等待接收队列 -> 条件是缓冲区为空且通道未关闭
*/

template <typename T> class Channel {
  // 发送Awaiter
  struct SendAwaiter {
    SendAwaiter(Channel &channel, T &value)
        : _channel{channel}, _value{value} {}

    // 总是挂起
    auto await_ready() const noexcept -> bool { return false; }

    auto await_suspend(std::coroutine_handle<> handle) -> bool {
      std::lock_guard lock(_channel._mutex, std::adopt_lock);
      _handle = handle;
      // 如果已经有等待着接受数据的receiver，则直接将当前值放入对应接收者的结果中，并恢复其协程
      if (!_channel._waiting_receivers.empty()) {
        auto receiver = _channel._waiting_receivers.front();
        _channel._waiting_receivers.pop_front();
        receiver->_result = std::move(_value);
        runtime::detail::push_task_to_local_queue(receiver->_handle);
        // 设置当前结果为成功
        _result.emplace();
        // 返回false，表示不挂起
        return false;
      }
      // 如果环形缓冲区已满，则返回true，表示挂起
      if (_channel._buffer.is_fill()) {
        // 如果通道已关闭，则返回false，表示不挂起
        if (_channel._is_closed.load(std::memory_order::acquire)) {
          return false;
        }
        // 如果缓冲区满了
        // 且通道未关闭，则将当前发送者加入等待发送者队列，等待receiver取走数据
        _channel._waiting_senders.push_back(this);
        // 返回true，表示挂起
        return true;
      } else {
        // 将当前值放入环形缓冲区
        _channel._buffer.safety_push(std::move(_value));
        // 设置当前结果为成功
        _result.emplace();
        return false;
      }
    }
    // 恢复逻辑：返回结果
    auto await_resume() const noexcept { return _result; }

  public:
    Channel &_channel;
    T &_value;
    expected<void> _result{std::unexpected{make_error(
        Error::ClosedChannel)}}; // 是不是成功发送了，或者是不是成功被接收了
    std::coroutine_handle<> _handle;
  };

  // 接受Awaiter
  struct RecvAwaiter {
    RecvAwaiter(Channel &channel) : _channel{channel} {}

    auto await_ready() const noexcept -> bool { return false; }

    // 挂起逻辑：
    //  如果等待发送者队列不为空，则取出一个发送者，将其结果设置为当前值，并恢复其协程
    //  如果环形缓冲区为空，则返回true，表示挂起
    //  如果环形缓冲区不为空，则将当前值移动到环形缓冲区中，并恢复当前协程
    //  如果通道已关闭，则返回false，表示不挂起
    auto await_suspend(std::coroutine_handle<> handle) -> bool {
      std::lock_guard lock(_channel._mutex, std::adopt_lock);
      _handle = handle;
      if (!_channel._waiting_senders.empty()) {
        auto sender = _channel._waiting_senders.front();
        _channel._waiting_senders.pop_front();
        // 设置sender的结果为成功
        sender->_result.emplace();
        // 将当前值从sender的_value中取走，恢复sender
        _result.emplace(std::move(sender->_value));
        runtime::detail::push_task_to_local_queue(sender->_handle);
        return false;
      }
      // 如果环形缓冲区为空，则返回true，表示挂起
      if (_channel._buffer.is_empty()) {
        // 如果通道已关闭，则返回false，表示不挂起
        if (_channel._is_closed.load(std::memory_order::acquire)) {
          return false;
        }
        // 将当前接收者加入等待接收者队列
        _channel._waiting_receivers.push_back(this);
        return true;
      } else {
        // 从环形缓冲区中取出值
        _result.emplace(_channel._buffer.safety_pop());
        // 设置当前结果为成功
        return false;
      }
    }

    // 恢复逻辑：返回结果
    auto await_resume() noexcept -> expected<T> { return std::move(_result); }

  public:
    Channel &_channel; // 通道
    expected<T> _result{
        std::unexpected{make_error(Error::ClosedChannel)}}; // 存放接收到的值
    std::coroutine_handle<> _handle;                        // 协程句柄
  };

public:
  // 值类型萃取，方便外部使用
  using ValueType = T;

  Channel(std::size_t num_senders, std::size_t num_receivers, std::size_t cap)
      : _num_senders{num_senders}, _num_receivers{num_receivers}, _buffer{cap} {
  }

public:
  // 发送数据,是一个协程
  auto send(T value) -> task<expected<void>> {
    // 加锁
    co_await _mutex.lock();
    auto res = co_await SendAwaiter{*this, value};
    co_return res;
  }

  // 接收数据,是一个协程
  auto recv() -> task<expected<T>> {
    co_await _mutex.lock();
    auto res = co_await RecvAwaiter{*this};
    co_return res;
  }

  void add_sender() { _num_senders.fetch_add(1, std::memory_order::relaxed); }

  void sub_sender() {
    if (_num_senders.fetch_sub(1, std::memory_order::acq_rel) == 1) {
      destroy();
    }
  }

  void add_receiver() {
    _num_receivers.fetch_add(1, std::memory_order::relaxed);
  }

  void sub_receiver() {
    if (_num_receivers.fetch_sub(1, std::memory_order::acq_rel) == 1) {
      destroy();
    }
  }

private:
  // 销毁通道
  void destroy() {
    if (_is_closed.exchange(true, std::memory_order::acq_rel) == false) {
      // 唤醒所有等待发送者
      for (auto sender : _waiting_senders) {
        runtime::detail::push_task_to_local_queue(sender->_handle);
      }
      // 唤醒所有等待接收者
      for (auto receiver : _waiting_receivers) {
        runtime::detail::push_task_to_local_queue(receiver->_handle);
      }
      // 清空等待发送者队列
      _waiting_senders.clear();
    }
  }

private:
  std::atomic<std::size_t> _num_senders;         // 发送者数量
  std::atomic<std::size_t> _num_receivers;       // 接收者数量
  RingBuffer<T> _buffer;                         // 环形缓冲区
  std::list<SendAwaiter *> _waiting_senders{};   // 等待发送者链
  std::list<RecvAwaiter *> _waiting_receivers{}; // 等待接收者链
  std::atomic<bool> _is_closed{false};           // 关闭标志
  mutex _mutex{};                                // 互斥锁
};

} // namespace faio::sync::detail

#endif // FAIO_DETAIL_SYNC__CHANNEL_CHANNEL_HPP