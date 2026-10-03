#pragma once
#include "faio/detail/io/util/algorithms.hpp"
#include "faio/detail/sync/condition_variable.hpp"
#include <atomic>
#include <cstring>

namespace faio::io {
/** @brief 内存字节流，读写共享逻辑位置，适合协议测试和通用算法。 */
class MemoryStream {
public:
  MemoryStream() = default;
  explicit MemoryStream(std::vector<char> bytes) : bytes_(std::move(bytes)) {}
  explicit MemoryStream(std::string_view bytes)
      : bytes_(bytes.begin(), bytes.end()) {}
  task<expected<std::size_t>> read(std::span<char> bytes) {
    const auto count = std::min(
        bytes.size(), bytes_.size() - std::min(position_, bytes_.size()));
    if (count)
      std::memcpy(bytes.data(), bytes_.data() + position_, count);
    position_ += count;
    co_return count;
  }
  task<expected<std::size_t>> write(std::span<const char> bytes) {
    if (bytes.size() > std::numeric_limits<std::size_t>::max() - position_)
      co_return std::unexpected{make_error(EOVERFLOW)};
    if (position_ + bytes.size() > bytes_.size())
      bytes_.resize(position_ + bytes.size());
    if (!bytes.empty())
      std::memcpy(bytes_.data() + position_, bytes.data(), bytes.size());
    position_ += bytes.size();
    co_return bytes.size();
  }
  task<expected<io_transfer>> read(io_buffer buffer) {
    auto count = co_await read(buffer.writable_bytes());
    if (!count)
      co_return std::unexpected{count.error()};
    buffer.set_size(*count);
    co_return io_transfer{std::move(buffer), *count};
  }
  task<expected<io_transfer>> write(io_buffer buffer) {
    auto count = co_await write(buffer.bytes());
    if (!count)
      co_return std::unexpected{count.error()};
    co_return io_transfer{std::move(buffer), *count};
  }
  task<expected<std::uint64_t>> seek(std::uint64_t position) {
    if (position > std::numeric_limits<std::size_t>::max())
      co_return std::unexpected{make_error(EOVERFLOW)};
    position_ = static_cast<std::size_t>(position);
    co_return position;
  }
  task<expected<void>> flush() { co_return expected<void>{}; }
  task<expected<void>> shutdown() { co_return expected<void>{}; }
  [[nodiscard]] const std::vector<char> &bytes() const noexcept {
    return bytes_;
  }

private:
  std::vector<char> bytes_;
  std::size_t position_{};
};
/** @brief EOF 读取源。 */
struct Empty {
  task<expected<std::size_t>> read(std::span<char>) {
    co_return std::size_t{0};
  }
};
/** @brief 无限重复单字节源；组合算法的上限和协作预算控制读取量。 */
struct Repeat {
  char value;
  task<expected<std::size_t>> read(std::span<char> bytes) {
    std::fill(bytes.begin(), bytes.end(), value);
    co_return bytes.size();
  }
};
/** @brief 接受并丢弃全部字节的写入端。 */
struct Sink {
  task<expected<std::size_t>> write(std::span<const char> bytes) {
    co_return bytes.size();
  }
  task<expected<void>> flush() { co_return expected<void>{}; }
  task<expected<void>> shutdown() { co_return expected<void>{}; }
};
inline Empty empty() noexcept { return {}; }
inline Repeat repeat(char byte) noexcept { return {byte}; }
inline Sink sink() noexcept { return {}; }

namespace detail {
/** @brief 有界全双工内存通道的单方向环形队列。 */
struct duplex_lane {
  explicit duplex_lane(std::size_t capacity) : buffer(capacity) {}
  std::vector<char> buffer;
  std::size_t head{}, size{};
  sync::mutex mutex;
  sync::condition_variable changed;
  std::atomic<bool> reader_closed{}, writer_closed{};
};
inline task<expected<std::size_t>>
duplex_read(std::shared_ptr<duplex_lane> lane, std::span<char> bytes) {
  if (!lane)
    co_return std::unexpected{make_error(EBADF)};
  if (bytes.empty())
    co_return std::size_t{0};
  try {
    auto guard = co_await lane->mutex.scoped_lock();
    co_await lane->changed.wait(
        lane->mutex, [&] { return lane->size || lane->writer_closed.load(); });
    const auto count = std::min(bytes.size(), lane->size);
    for (std::size_t i = 0; i < count; ++i)
      bytes[i] = lane->buffer[(lane->head + i) % lane->buffer.size()];
    lane->head = (lane->head + count) % lane->buffer.size();
    lane->size -= count;
    lane->changed.notify_all();
    co_return count;
  } catch (const operation_cancelled &) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}
inline task<expected<std::size_t>>
duplex_write(std::shared_ptr<duplex_lane> lane, std::span<const char> bytes) {
  if (!lane)
    co_return std::unexpected{make_error(EBADF)};
  if (bytes.empty())
    co_return std::size_t{0};
  try {
    auto guard = co_await lane->mutex.scoped_lock();
    co_await lane->changed.wait(lane->mutex, [&] {
      return lane->size < lane->buffer.size() || lane->reader_closed.load() ||
             lane->writer_closed.load();
    });
    if (lane->reader_closed.load() || lane->writer_closed.load())
      co_return std::unexpected{make_error(EPIPE)};
    const auto count = std::min(bytes.size(), lane->buffer.size() - lane->size);
    for (std::size_t i = 0; i < count; ++i)
      lane->buffer[(lane->head + lane->size + i) % lane->buffer.size()] =
          bytes[i];
    lane->size += count;
    lane->changed.notify_all();
    co_return count;
  } catch (const operation_cancelled &) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}
} // namespace detail
/** @brief 有界内存全双工端；写满时挂起，写半关闭后对端排空并读取 EOF。 */
class DuplexStream {
public:
  DuplexStream(std::shared_ptr<detail::duplex_lane> input,
               std::shared_ptr<detail::duplex_lane> output)
      : input_(std::move(input)), output_(std::move(output)) {}
  DuplexStream(const DuplexStream &) = delete;
  DuplexStream &operator=(const DuplexStream &) = delete;
  DuplexStream(DuplexStream &&) noexcept = default;
  DuplexStream &operator=(DuplexStream &&other) noexcept {
    if (this != &other) {
      close_lanes();
      input_ = std::move(other.input_);
      output_ = std::move(other.output_);
    }
    return *this;
  }
  ~DuplexStream() { close_lanes(); }
  auto read(std::span<char> bytes) {
    return detail::duplex_read(input_, bytes);
  }
  auto write(std::span<const char> bytes) {
    return detail::duplex_write(output_, bytes);
  }
  task<expected<void>> flush() { co_return expected<void>{}; }
  task<expected<void>> shutdown_write() {
    if (output_) {
      output_->writer_closed.store(true);
      output_->changed.notify_all();
    }
    co_return expected<void>{};
  }
  auto shutdown() { return shutdown_write(); }

private:
  void close_lanes() noexcept {
    if (input_) {
      input_->reader_closed.store(true);
      input_->changed.notify_all();
    }
    if (output_) {
      output_->writer_closed.store(true);
      output_->changed.notify_all();
    }
  }
  std::shared_ptr<detail::duplex_lane> input_, output_;
};
inline std::pair<DuplexStream, DuplexStream>
duplex(std::size_t capacity = 65536) {
  if (!capacity)
    throw std::invalid_argument("duplex容量必须大于零");
  auto first = std::make_shared<detail::duplex_lane>(capacity);
  auto second = std::make_shared<detail::duplex_lane>(capacity);
  return {DuplexStream{first, second}, DuplexStream{second, first}};
}
} // namespace faio::io
