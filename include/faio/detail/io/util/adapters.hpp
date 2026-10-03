#pragma once
#include "faio/detail/io/util/algorithms.hpp"
#include "faio/detail/sync/mutex.hpp"

namespace faio::io {
/** @brief 只读取底层流的前 limit 字节；达到上限立即 EOF，不额外消耗底层。 */
template <borrowed_async_reader Reader>
class Take {
 public:
  Take(Reader& reader, std::uint64_t limit) : reader_(reader), limit_(limit) {}

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
  Reader& reader_;
  std::uint64_t limit_;
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
  A& first_;
  B& second_;
  bool first_done_{};
};

template <borrowed_async_reader A, borrowed_async_reader B>
auto chain(A& first, B& second) {
  return Chain<A, B>{first, second};
}

namespace detail {
template <class Stream>
struct split_state {
  explicit split_state(Stream value) : stream(std::move(value)) {}

  Stream stream;
  sync::mutex mutex;  // 泛型流没有专属全双工gate，只能通过异步锁安全串行。
};
}  // namespace detail

template <class Stream>
class ReadHalf {
 public:
  explicit ReadHalf(std::shared_ptr<detail::split_state<Stream>> state)
      : state_(std::move(state)) {}

  task<expected<std::size_t>> read(std::span<char> bytes) {
    try {
      auto guard = co_await state_->mutex.scoped_lock();
      co_return co_await state_->stream.read(bytes);
    } catch (const operation_cancelled&) {
      co_return std::unexpected{make_error(ECANCELED)};
    }
  }

 private:
  std::shared_ptr<detail::split_state<Stream>> state_;
};

template <class Stream>
class WriteHalf {
 public:
  explicit WriteHalf(std::shared_ptr<detail::split_state<Stream>> state)
      : state_(std::move(state)) {}

  task<expected<std::size_t>> write(std::span<const char> bytes) {
    try {
      auto guard = co_await state_->mutex.scoped_lock();
      co_return co_await state_->stream.write(bytes);
    } catch (const operation_cancelled&) {
      co_return std::unexpected{make_error(ECANCELED)};
    }
  }

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
  std::shared_ptr<detail::split_state<Stream>> state_;
};

/** @brief 消费一个可移动泛型流，半边共有控制块；TCP请使用专属split保持全双工。
 */
template <class Stream>
  requires borrowed_async_reader<Stream> && borrowed_async_writer<Stream>
auto split(Stream stream) {
  auto state = std::make_shared<detail::split_state<Stream>>(std::move(stream));
  return std::pair{ReadHalf<Stream>{state}, WriteHalf<Stream>{state}};
}
}  // namespace faio::io
