/**
 * @file faio/detail/io/stream/memory_stream.hpp
 * @brief 跨平台异步 IO：内存流与异步双工通道。
 * @details 统一 IO 通过上下文、资源租约与等待器连接运行时；组合操作保留部分进度并在安全完成后归还负载及方向执行权。
 */
#pragma once
#include "faio/detail/io/stream/algorithms.hpp"
#include "faio/detail/sync/condition_variable.hpp"
#include <atomic>
#include <cstring>

namespace faio::io {
/** @brief 内存字节流，读写共享逻辑位置，适合协议测试和通用算法。 */
class MemoryStream {
 public:
  MemoryStream() = default;

  explicit MemoryStream(std::vector<char> bytes) : bytes_(std::move(bytes)) {}

  explicit MemoryStream(std::string_view bytes) : bytes_(bytes.begin(), bytes.end()) {}

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  task<expected<std::size_t>> read(std::span<char> bytes) {
    const auto count = std::min(bytes.size(), bytes_.size() - std::min(position_, bytes_.size()));
    if (count)
      std::memcpy(bytes.data(), bytes_.data() + position_, count);
    position_ += count;
    co_return count;
  }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
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

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  task<expected<io_transfer>> read(io_buffer buffer) {
    auto count = co_await read(buffer.writable_bytes());
    if (!count)
      co_return std::unexpected{count.error()};
    buffer.set_size(*count);
    co_return io_transfer{std::move(buffer), *count};
  }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  task<expected<io_transfer>> write(io_buffer buffer) {
    auto count = co_await write(buffer.bytes());
    if (!count)
      co_return std::unexpected{count.error()};
    co_return io_transfer{std::move(buffer), *count};
  }

  /// @brief 按给定定位基准与偏移更新文件逻辑游标。
  task<expected<std::uint64_t>> seek(std::uint64_t position) {
    if (position > std::numeric_limits<std::size_t>::max())
      co_return std::unexpected{make_error(EOVERFLOW)};
    position_ = static_cast<std::size_t>(position);
    co_return position;
  }

  /// @brief 提交缓冲的写入或已准备的后端请求，失败保留尚未完成的责任。
  task<expected<void>> flush() { co_return expected<void>{}; }

  /// @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。
  task<expected<void>> shutdown() { co_return expected<void>{}; }

  /// @brief 返回内部字节存储的视图；视图不延长对象或负载生命期。
  [[nodiscard]] const std::vector<char>& bytes() const noexcept { return bytes_; }

 private:
  std::vector<char> bytes_;  ///< 内存流拥有的字节存储。
  std::size_t position_{};  ///< 当前读写游标的字节偏移。
};

/** @brief EOF 读取源。 */
struct Empty {
  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  task<expected<std::size_t>> read(std::span<char>) { co_return std::size_t{0}; }
};

/** @brief 无限重复单字节源；组合算法的上限和协作预算控制读取量。 */
struct Repeat {
  char value;  ///< 持有当前结果值或快照，读取前须确认操作已完成。

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  task<expected<std::size_t>> read(std::span<char> bytes) {
    std::fill(bytes.begin(), bytes.end(), value);
    co_return bytes.size();
  }
};

/** @brief 接受并丢弃全部字节的写入端。 */
struct Sink {
  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  task<expected<std::size_t>> write(std::span<const char> bytes) { co_return bytes.size(); }

  /// @brief 提交缓冲的写入或已准备的后端请求，失败保留尚未完成的责任。
  task<expected<void>> flush() { co_return expected<void>{}; }

  /// @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。
  task<expected<void>> shutdown() { co_return expected<void>{}; }
};

/// @brief 检查当前对象是否没有可消费的数据或任务。
inline Empty empty() noexcept {
  return {};
}

inline Repeat repeat(char byte) noexcept {
  return {byte};
}

inline Sink sink() noexcept {
  return {};
}

namespace detail {
/** @brief 有界全双工内存通道的单方向环形队列。 */
struct duplex_lane {
  explicit duplex_lane(std::size_t capacity) : buffer(capacity) {}

  std::vector<char> buffer;  ///< 对象拥有的字节缓冲区，跨异步挂起保持内容与地址要求。
  std::size_t head{},  ///< 下一条待读数据或节点的起点。
      size{};  ///< 当前有效数据的数量。
  sync::mutex mutex;  ///< 串行保护所属对象的共享状态；调度与用户回调应遵循该对象的锁外调用协议。
  sync::condition_variable changed;  ///< 内存双工通道数据或关闭状态变化时使用的异步条件变量。
  std::atomic<bool> reader_closed{},  ///< 读端是否已关闭，写入方据此报告对端不可再接收。
      writer_closed{};  ///< 写端是否已关闭，读取方排空剩余数据后观察 EOF。
};

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
}

/** @brief 有界内存全双工端；写满时挂起，写半关闭后对端排空并读取 EOF。 */
class DuplexStream {
 public:
  DuplexStream(std::shared_ptr<detail::duplex_lane> input,
               std::shared_ptr<detail::duplex_lane> output)
      : input_(std::move(input)), output_(std::move(output)) {}

  DuplexStream(const DuplexStream&) = delete;

  DuplexStream& operator=(const DuplexStream&) = delete;

  DuplexStream(DuplexStream&&) noexcept = default;

  DuplexStream& operator=(DuplexStream&& other) noexcept {
    if (this != &other) {
      close_lanes();
      input_ = std::move(other.input_);
      output_ = std::move(other.output_);
    }
    return *this;
  }

  ~DuplexStream() { close_lanes(); }

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  auto read(std::span<char> bytes) { return detail::duplex_read(input_, bytes); }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  auto write(std::span<const char> bytes) { return detail::duplex_write(output_, bytes); }

  /// @brief 提交缓冲的写入或已准备的后端请求，失败保留尚未完成的责任。
  task<expected<void>> flush() { co_return expected<void>{}; }

  /// @brief 请求安全写半关闭，不隐含关闭读方向或伪造读 EOF。
  task<expected<void>> shutdown_write() {
    if (output_) {
      output_->writer_closed.store(true);
      output_->changed.notify_all();
    }
    co_return expected<void>{};
  }

  /// @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。
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

  std::shared_ptr<detail::duplex_lane> input_,  ///< 当前端读取的共享内存通道。
      output_;  ///< 当前端写入的共享内存通道。
};

inline std::pair<DuplexStream, DuplexStream> duplex(std::size_t capacity = 65536) {
  if (!capacity)
    throw std::invalid_argument("duplex容量必须大于零");
  auto first = std::make_shared<detail::duplex_lane>(capacity);
  auto second = std::make_shared<detail::duplex_lane>(capacity);
  return {DuplexStream{first, second}, DuplexStream{second, first}};
}
}
