/**
 * @file faio/detail/io/stream/memory_stream.hpp
 * @brief 内存流与异步双工通道。
 */
#pragma once
#include "faio/detail/io/stream/algorithms.hpp"
#include "faio/detail/sync/condition_variable.hpp"
#include <atomic>
#include <cstring>

namespace faio::io {
/**  @brief 内存字节流，读写共享逻辑位置，适合协议测试和通用算法。 */
class memory_stream {
 public:
  /** @brief 构造内存字节流。 */
  memory_stream() = default;

  /** @brief 构造内存字节流。 */
  explicit memory_stream(std::vector<char> bytes) : bytes_(std::move(bytes)) {}

  /** @brief 构造内存字节流。 */
  explicit memory_stream(std::string_view bytes) : bytes_(bytes.begin(), bytes.end()) {}

  /** @brief 从当前游标读取字节，返回实际读取量；非空请求在末尾返回零。 */
  task<expected<std::size_t>> read(std::span<char> bytes) {
    const auto count = std::min(bytes.size(), bytes_.size() - std::min(position_, bytes_.size()));
    if (count)
      std::memcpy(bytes.data(), bytes_.data() + position_, count);
    position_ += count;
    co_return count;
  }

  /** @brief 从当前游标写入字节，必要时扩展存储，返回实际写入量。 */
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

  /** @brief 将读取结果写入接管的缓冲区，返回缓冲区及实际读取量。 */
  task<expected<io_transfer>> read(io_buffer buffer) {
    auto count = co_await read(buffer.writable_bytes());
    if (!count)
      co_return std::unexpected{count.error()};
    buffer.set_size(*count);
    co_return io_transfer{std::move(buffer), *count};
  }

  /** @brief 写入接管缓冲区中的有效字节，返回缓冲区及实际写入量。 */
  task<expected<io_transfer>> write(io_buffer buffer) {
    auto count = co_await write(buffer.bytes());
    if (!count)
      co_return std::unexpected{count.error()};
    co_return io_transfer{std::move(buffer), *count};
  }

  /** @brief 将内存流的逻辑游标设为指定绝对位置。 */
  task<expected<std::uint64_t>> seek(std::uint64_t position) {
    if (position > std::numeric_limits<std::size_t>::max())
      co_return std::unexpected{make_error(EOVERFLOW)};
    position_ = static_cast<std::size_t>(position);
    co_return position;
  }

  /** @brief 完成刷新；该类型没有待提交的写缓存。 */
  task<expected<void>> flush() { co_return expected<void>{}; }

  /** @brief 直接返回成功，无需执行关闭操作。 */
  task<expected<void>> shutdown() { co_return expected<void>{}; }

  /** @brief 返回内部字节存储的视图；视图不延长对象或负载的生命周期。 */
  [[nodiscard]] const std::vector<char>& bytes() const noexcept { return bytes_; }

 private:
  std::vector<char> bytes_; /**< 内存流拥有的字节存储。 */
  std::size_t position_{};  /**< 当前读写游标的字节偏移。 */
};

/**  @brief EOF 读取源。 */
struct empty_reader {
  /** @brief 直接返回零，表示 EOF。 */
  task<expected<std::size_t>> read(std::span<char>) { co_return std::size_t{0}; }
};

/**  @brief 无限重复单字节源；组合算法的上限和协作预算控制读取量。 */
struct repeat_reader {
  char value; /**< 每次读取时重复填充的字节值。 */

  /** @brief 用 value 填满输出缓冲区，返回填充字节数。 */
  task<expected<std::size_t>> read(std::span<char> bytes) {
    std::fill(bytes.begin(), bytes.end(), value);
    co_return bytes.size();
  }
};

/**  @brief 接受并丢弃全部字节的写入端。 */
struct sink_writer {
  /** @brief 丢弃输入字节，并报告全部写入完成。 */
  task<expected<std::size_t>> write(std::span<const char> bytes) { co_return bytes.size(); }

  /** @brief 完成刷新；该类型没有待提交的写缓存。 */
  task<expected<void>> flush() { co_return expected<void>{}; }

  /** @brief 直接返回成功，无需执行关闭操作。 */
  task<expected<void>> shutdown() { co_return expected<void>{}; }
};

/** @brief 创建始终返回 EOF 的读取源。 */
inline empty_reader empty() noexcept {
  return {};
}

/** @brief 创建重复返回指定字节的输入流。 */
inline repeat_reader repeat(char byte) noexcept {
  return {byte};
}

/** @brief 创建接收并丢弃字节的输出流。 */
inline sink_writer sink() noexcept {
  return {};
}

namespace detail {
/**  @brief 有界全双工内存通道的单方向环形队列。 */
struct duplex_lane {
  /** @brief 构造内存双工流的单向通道。 */
  explicit duplex_lane(std::size_t capacity) : buffer(capacity) {}

  std::vector<char> buffer;         /**< 对象持有的字节缓冲区，在挂起期间保持所需内容和地址有效。 */
  std::size_t head{}, size{};       /**< 当前有效数据的数量。 */
  sync::mutex mutex;                /**< 保护环形缓冲区和关闭状态；通知及回调遵循锁外调用约定。 */
  sync::condition_variable changed; /**< 内存双工通道数据或关闭状态变化时使用的异步条件变量。 */
  std::atomic<bool> reader_closed{},
      writer_closed{}; /**< 写端是否已关闭，读取方排空剩余数据后观察 EOF。 */
};

/** @brief 从内存双工通道读取字节。 */
inline task<expected<std::size_t>> duplex_read(std::shared_ptr<duplex_lane> lane,
                                               std::span<char> bytes) {
  if (!lane)
    co_return std::unexpected{make_error(EBADF)};
  if (bytes.empty())
    co_return std::size_t{0};
  try {
    auto guard = co_await lane->mutex.scoped_lock();
    co_await lane->changed.wait(lane->mutex,
                                [&] { return lane->size || lane->writer_closed.load(); });
    const auto count = std::min(bytes.size(), lane->size);
    for (std::size_t i = 0; i < count; ++i)
      bytes[i] = lane->buffer[(lane->head + i) % lane->buffer.size()];
    lane->head = (lane->head + count) % lane->buffer.size();
    lane->size -= count;
    lane->changed.notify_all();
    co_return count;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}

/** @brief 向内存双工通道写入字节。 */
inline task<expected<std::size_t>> duplex_write(std::shared_ptr<duplex_lane> lane,
                                                std::span<const char> bytes) {
  if (!lane)
    co_return std::unexpected{make_error(EBADF)};
  if (bytes.empty())
    co_return std::size_t{0};
  try {
    auto guard = co_await lane->mutex.scoped_lock();
    co_await lane->changed.wait(lane->mutex, [&] {
      return lane->size < lane->buffer.size() || lane->reader_closed.load()
             || lane->writer_closed.load();
    });
    if (lane->reader_closed.load() || lane->writer_closed.load())
      co_return std::unexpected{make_error(EPIPE)};
    const auto count = std::min(bytes.size(), lane->buffer.size() - lane->size);
    for (std::size_t i = 0; i < count; ++i)
      lane->buffer[(lane->head + lane->size + i) % lane->buffer.size()] = bytes[i];
    lane->size += count;
    lane->changed.notify_all();
    co_return count;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}
}  // namespace detail

/**  @brief 有界内存全双工端；写满时挂起，写半关闭后对端排空并读取 EOF。 */
class duplex_stream {
 public:
  /** @brief 构造内存双工流。 */
  duplex_stream(std::shared_ptr<detail::duplex_lane> input,
                std::shared_ptr<detail::duplex_lane> output)
      : input_(std::move(input)), output_(std::move(output)) {}

  /** @brief 禁止复制构造内存双工流。 */
  duplex_stream(const duplex_stream&) = delete;

  /** @brief 禁止复制赋值。 */
  duplex_stream& operator=(const duplex_stream&) = delete;

  /** @brief 移动构造内存双工流。 */
  duplex_stream(duplex_stream&&) noexcept = default;

  /** @brief 移动赋值。 */
  duplex_stream& operator=(duplex_stream&& other) noexcept {
    if (this != &other) {
      close_lanes();
      input_ = std::move(other.input_);
      output_ = std::move(other.output_);
    }
    return *this;
  }

  /** @brief 销毁内存双工流。 */
  ~duplex_stream() { close_lanes(); }

  /** @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。 */
  auto read(std::span<char> bytes) { return detail::duplex_read(input_, bytes); }

  /** @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。 */
  auto write(std::span<const char> bytes) { return detail::duplex_write(output_, bytes); }

  /** @brief 完成刷新；该类型没有待提交的写缓存。 */
  task<expected<void>> flush() { co_return expected<void>{}; }

  /** @brief 请求安全写半关闭，保持读方向独立，不产生额外的读 EOF。 */
  task<expected<void>> shutdown_write() {
    if (output_) {
      output_->writer_closed.store(true);
      output_->changed.notify_all();
    }
    co_return expected<void>{};
  }

  /** @brief 完成写端关闭。 */
  auto shutdown() { return shutdown_write(); }

 private:
  /** @brief 关闭资源的方向通道。 */
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

  std::shared_ptr<detail::duplex_lane> input_, output_; /**< 当前端写入的共享内存通道。 */
};

/** @brief 创建配对的内存双工流端点。 */
inline std::pair<duplex_stream, duplex_stream> duplex(std::size_t capacity = 65536) {
  if (!capacity)
    throw std::invalid_argument("duplex容量必须大于零");
  auto first = std::make_shared<detail::duplex_lane>(capacity);
  auto second = std::make_shared<detail::duplex_lane>(capacity);
  return {duplex_stream{first, second}, duplex_stream{second, first}};
}
}  // namespace faio::io
