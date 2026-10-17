/**
 * @file faio/detail/io/stream/adapters.hpp
 * @brief 跨平台异步 IO：输入输出适配器。
 * @details 统一 IO 通过上下文、资源租约与等待器连接运行时；组合操作保留部分进度并在安全完成后归还负载及方向执行权。
 */
#pragma once
#include "faio/detail/io/stream/algorithms.hpp"
#include "faio/detail/sync/mutex.hpp"

namespace faio::io {
/** @brief 只读取底层流的前 limit 字节；达到上限立即 EOF，不额外消耗底层。 */
template <borrowed_async_reader Reader>
class Take {
 public:
  Take(Reader& reader, std::uint64_t limit) : reader_(reader), limit_(limit) {}

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  task<expected<std::size_t>> read(std::span<char> bytes) {
    if (!limit_ || bytes.empty())
      co_return std::size_t{0};
    auto count = co_await reader_.read(
        bytes.first(static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), limit_))));
    if (count) {
      if (*count > std::min<std::uint64_t>(bytes.size(), limit_))
        co_return std::unexpected{make_error(EIO)};
      limit_ -= *count;
    }
    co_return count;
  }

  [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }

  void set_limit(std::uint64_t limit) noexcept { limit_ = limit; }

 private:
  Reader& reader_;  ///< 关联的底层读取或写入对象，所有权由成员声明的值或引用类型决定。
  std::uint64_t limit_;  ///< 本适配器还允许读取的最大字节数。
};

template <borrowed_async_reader Reader>
auto take(Reader& reader, std::uint64_t limit) {
  return Take<Reader>{reader, limit};
}

/** @brief 第一流 EOF 后继续第二流；零长度读取不会误切换第一流。 */
template <borrowed_async_reader A, borrowed_async_reader B>
class Chain {
 public:
  Chain(A& first, B& second) : first_(first), second_(second) {}

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  task<expected<std::size_t>> read(std::span<char> bytes) {
    if (bytes.empty())
      co_return std::size_t{0};
    if (!first_done_) {
      auto count = co_await first_.read(bytes);
      if (!count || *count)
        co_return count;
      first_done_ = true;
    }
    co_return co_await second_.read(bytes);
  }

 private:
  A& first_;  ///< 首先读取的输入流引用，其生命期由调用方保证。
  B& second_;  ///< 第一个输入耗尽后继续读取的第二个输入流引用。
  bool first_done_{};  ///< 第一个输入是否已经到达末尾。
};

template <borrowed_async_reader A, borrowed_async_reader B>
auto chain(A& first, B& second) {
  return Chain<A, B>{first, second};
}

namespace detail {
template <class Stream>
struct split_state {
  explicit split_state(Stream value) : stream(std::move(value)) {}

  Stream stream;  ///< 由读写拆分状态共享持有的底层双向流。
  sync::mutex mutex;  ///< 泛型流没有专属全双工gate，只能通过异步锁安全串行。
};
}

template <class Stream>
class ReadHalf {
 public:
  explicit ReadHalf(std::shared_ptr<detail::split_state<Stream>> state)
      : state_(std::move(state)) {}

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  task<expected<std::size_t>> read(std::span<char> bytes) {
    try {
      auto guard = co_await state_->mutex.scoped_lock();
      co_return co_await state_->stream.read(bytes);
    } catch (const operation_cancelled&) {
      co_return std::unexpected{make_error(ECANCELED)};
    }
  }

 private:
  std::shared_ptr<detail::split_state<Stream>> state_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
};

template <class Stream>
class WriteHalf {
 public:
  explicit WriteHalf(std::shared_ptr<detail::split_state<Stream>> state)
      : state_(std::move(state)) {}

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  task<expected<std::size_t>> write(std::span<const char> bytes) {
    try {
      auto guard = co_await state_->mutex.scoped_lock();
      co_return co_await state_->stream.write(bytes);
    } catch (const operation_cancelled&) {
      co_return std::unexpected{make_error(ECANCELED)};
    }
  }

  /// @brief 提交缓冲的写入或已准备的后端请求，失败保留尚未完成的责任。
  task<expected<void>> flush() {
    try {
      auto guard = co_await state_->mutex.scoped_lock();
      if constexpr (requires { state_->stream.flush(); })
        co_return co_await state_->stream.flush();
      co_return expected<void>{};
    } catch (const operation_cancelled&) {
      co_return std::unexpected{make_error(ECANCELED)};
    }
  }

 private:
  std::shared_ptr<detail::split_state<Stream>> state_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
};

/** @brief 消费一个可移动泛型流，半边共有控制块；TCP请使用专属split保持全双工。
 */
template <class Stream>
  requires borrowed_async_reader<Stream> && borrowed_async_writer<Stream>
auto split(Stream stream) {
  auto state = std::make_shared<detail::split_state<Stream>>(std::move(stream));
  return std::pair{ReadHalf<Stream>{state}, WriteHalf<Stream>{state}};
}
}
