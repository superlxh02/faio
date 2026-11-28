/**
 * @file faio/detail/io/stream/adapters.hpp
 * @brief 输入输出适配器。
 */
#pragma once
#include "faio/detail/io/stream/algorithms.hpp"
#include "faio/detail/sync/mutex.hpp"

namespace faio::io {
/**  @brief 最多读取底层流的前 limit 字节；达到上限即返回 EOF，不再消费底层数据。 */
template <borrowed_async_reader Reader>
class limited_reader {
 public:
  /** @brief 构造限量读取适配器。 */
  limited_reader(Reader& reader, std::uint64_t limit) : reader_(reader), limit_(limit) {}

  /** @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。 */
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

  /** @brief 返回剩余可读取的字节上限。 */
  [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }

  /** @brief 设置剩余可读取的字节上限。 */
  void set_limit(std::uint64_t limit) noexcept { limit_ = limit; }

 private:
  Reader& reader_;      /**< 关联的底层读取或写入对象，所有权由成员声明的值或引用类型决定。 */
  std::uint64_t limit_; /**< 本适配器还允许读取的最大字节数。 */
};

/** @brief 创建最多读取 limit 字节的适配器。 */
template <borrowed_async_reader Reader>
auto take(Reader& reader, std::uint64_t limit) {
  return limited_reader<Reader>{reader, limit};
}

/**  @brief 第一个流到达 EOF 后继续读取第二个流；零长度读取不触发切换。 */
template <borrowed_async_reader A, borrowed_async_reader B>
class chained_reader {
 public:
  /** @brief 构造串联输入流。 */
  chained_reader(A& first, B& second) : first_(first), second_(second) {}

  /** @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。 */
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
  A& first_;          /**< 首先读取的输入流引用，其生命周期由调用方保证。 */
  B& second_;         /**< 第一个输入耗尽后继续读取的第二个输入流引用。 */
  bool first_done_{}; /**< 第一个输入是否已经到达末尾。 */
};

/** @brief 依次读取两个输入流。 */
template <borrowed_async_reader A, borrowed_async_reader B>
auto chain(A& first, B& second) {
  return chained_reader<A, B>{first, second};
}

namespace detail {
/** @brief 拆分流读写半部的共享状态。 */
template <class Stream>
struct split_state {
  /** @brief 构造拆分流读写半部的共享状态。 */
  explicit split_state(Stream value) : stream(std::move(value)) {}

  Stream stream;     /**< 由读写拆分状态共享持有的底层双向流。 */
  sync::mutex mutex; /**< 泛型流没有专属的全双工门闩，读写须通过异步锁串行执行。 */
};
}  // namespace detail

/** @brief 拆分流的读取半部。 */
template <class Stream>
class read_half {
 public:
  /** @brief 构造拆分流的读取半部。 */
  explicit read_half(std::shared_ptr<detail::split_state<Stream>> state)
      : state_(std::move(state)) {}

  /** @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。 */
  task<expected<std::size_t>> read(std::span<char> bytes) {
    try {
      auto guard = co_await state_->mutex.scoped_lock();
      co_return co_await state_->stream.read(bytes);
    } catch (const operation_cancelled&) {
      co_return std::unexpected{make_error(ECANCELED)};
    }
  }

 private:
  std::shared_ptr<detail::split_state<Stream>>
      state_; /**< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。 */
};

/** @brief 拆分流的写入半部。 */
template <class Stream>
class write_half {
 public:
  /** @brief 构造拆分流的写入半部。 */
  explicit write_half(std::shared_ptr<detail::split_state<Stream>> state)
      : state_(std::move(state)) {}

  /** @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。 */
  task<expected<std::size_t>> write(std::span<const char> bytes) {
    try {
      auto guard = co_await state_->mutex.scoped_lock();
      co_return co_await state_->stream.write(bytes);
    } catch (const operation_cancelled&) {
      co_return std::unexpected{make_error(ECANCELED)};
    }
  }

  /** @brief 刷新底层流的缓冲写入，失败时保留未完成的写入。 */
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
  std::shared_ptr<detail::split_state<Stream>>
      state_; /**< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。 */
};

/**  @brief 接管一个可移动的泛型流，读写半部共享控制块；TCP 须使用专属 split 接口保持全双工。 */
template <class Stream>
  requires borrowed_async_reader<Stream> && borrowed_async_writer<Stream>
auto split(Stream stream) {
  auto state = std::make_shared<detail::split_state<Stream>>(std::move(stream));
  return std::pair{read_half<Stream>{state}, write_half<Stream>{state}};
}
}  // namespace faio::io
