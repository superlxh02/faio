/**
 * @file faio/detail/io/stream/buffered.hpp
 * @brief 跨平台异步 IO：读写缓冲与分隔符读取。
 * @details 统一 IO 通过上下文、资源租约与等待器连接运行时；组合操作保留部分进度并在安全完成后归还负载及方向执行权。
 */
#pragma once
#include "faio/detail/io/stream/algorithms.hpp"
#include <cstring>
#include <optional>

namespace faio::io {
/** @brief 有界预读适配器；底层 Reader 必须覆盖适配器和全部等待操作。
 * @details 不允许直接从底层读取绕过尚未消费的预读内容。fill_buf 返回的借用
 * 视图在 consume 或下一次读取后失效。单方向使用，与底层相同地禁止并发读。
 */
template <borrowed_async_reader Reader>
class BufReader {
 public:
  explicit BufReader(Reader& reader, std::size_t capacity = 8192)
      : reader_(reader), buffer_(capacity) {
    if (!capacity)
      throw std::invalid_argument("BufReader容量必须大于零");
  }

  /// @brief 提供尚未消费的读缓冲区视图，必要时先推进底层读取。
  task<expected<std::span<const char>>> fill_buf() {
    if (position_ == end_) {
      auto count = co_await reader_.read(buffer_);
      if (!count)
        co_return std::unexpected{count.error()};
      if (*count > buffer_.size())
        co_return std::unexpected{make_error(EIO)};
      position_ = 0;
      end_ = *count;
    }
    co_return std::span<const char>{buffer_}.subspan(position_, end_ - position_);
  }

  /// @brief 确认已消费缓冲区前缀，并推进读取位置。
  void consume(std::size_t count) {
    if (count > end_ - position_)
      throw std::out_of_range("BufReader消费超过已填充范围");
    position_ += count;
  }

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  task<expected<std::size_t>> read(std::span<char> output) {
    if (output.empty())
      co_return std::size_t{0};
    if (position_ == end_ && output.size() >= buffer_.size())
      co_return co_await reader_.read(output);
    auto available = co_await fill_buf();
    if (!available)
      co_return std::unexpected{available.error()};
    const auto count = std::min(output.size(), available->size());
    if (count)
      std::memcpy(output.data(), available->data(), count);
    consume(count);
    co_return count;
  }

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  task<expected<io_transfer>> read(io_buffer output) {
    auto count = co_await read(output.writable_bytes());
    if (!count)
      co_return std::unexpected{count.error()};
    output.set_size(*count);
    co_return io_transfer{std::move(output), *count};
  }

  [[nodiscard]] std::span<const char> buffered() const noexcept {
    return std::span<const char>{buffer_}.subspan(position_, end_ - position_);
  }

  Reader& get_ref() noexcept { return reader_; }

 private:
  Reader& reader_;  ///< 关联的底层读取或写入对象，所有权由成员声明的值或引用类型决定。
  std::vector<char> buffer_;  ///< 对象拥有的字节缓冲区，跨异步挂起保持内容与地址要求。
  std::size_t position_{},  ///< 当前读写游标的字节偏移。
      end_{};  ///< 读缓冲区有效数据的结束偏移，不含该位置。
};

/** @brief 有界缓冲写适配器；必须显式 flush/shutdown，析构不执行异步 IO。
 * @details 写成功表示字节进入缓冲或底层；flush 出错保留未写出的字节。
 * pending_bytes/take_pending 可恢复未提交内容，析构不会冒充 flush 成功。
 */
template <borrowed_async_writer Writer>
class BufWriter {
 public:
  explicit BufWriter(Writer& writer, std::size_t capacity = 8192)
      : writer_(writer), capacity_(capacity) {
    if (!capacity)
      throw std::invalid_argument("BufWriter容量必须大于零");
    buffer_.reserve(capacity);
  }

  /// @brief 提交缓冲的写入或已准备的后端请求，失败保留尚未完成的责任。
  task<expected<void>> flush() {
    std::size_t total = 0;
    while (total != buffer_.size()) {
      auto count = co_await writer_.write(std::span<const char>{buffer_}.subspan(total));
      if (!count || !*count || *count > buffer_.size() - total) {
        const auto error = !count ? count.error() : Error{!*count ? Error::WriteZero : EIO};
        const auto committed =
            total + std::min<std::size_t>(error.progress(), buffer_.size() - total);
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(committed));
        co_return std::unexpected{Error{error.value(), total + error.progress(), error.domain()}};
      }
      total += *count;
      co_await this_coro::yield_if_needed();
    }
    buffer_.clear();
    if constexpr (requires { writer_.flush(); })
      co_return co_await writer_.flush();
    co_return expected<void>{};
  }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  task<expected<std::size_t>> write(std::span<const char> input) {
    if (input.empty())
      co_return std::size_t{0};
    if (input.size() > capacity_ - buffer_.size()) {
      auto flushed = co_await flush();
      if (!flushed)
        co_return std::unexpected{flushed.error()};
    }
    if (input.size() >= capacity_)
      co_return co_await writer_.write(input);
    buffer_.insert(buffer_.end(), input.begin(), input.end());
    co_return input.size();
  }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  task<expected<io_transfer>> write(io_buffer input) {
    auto count = co_await write(input.bytes());
    if (!count)
      co_return std::unexpected{count.error()};
    co_return io_transfer{std::move(input), *count};
  }

  /// @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。
  task<expected<void>> shutdown() {
    auto flushed = co_await flush();
    if (!flushed)
      co_return flushed;
    if constexpr (requires { writer_.shutdown_write(); })
      co_return co_await writer_.shutdown_write();
    else if constexpr (requires { writer_.shutdown(); })
      co_return co_await writer_.shutdown();
    co_return expected<void>{};
  }

  [[nodiscard]] std::span<const char> pending_bytes() const noexcept { return buffer_; }

  std::vector<char> take_pending() {
    auto bytes = std::move(buffer_);
    buffer_.clear();
    buffer_.reserve(capacity_);
    return bytes;
  }

  Writer& get_ref() noexcept { return writer_; }

 private:
  Writer& writer_;  ///< 关联的底层读取或写入对象，所有权由成员声明的值或引用类型决定。
  std::size_t capacity_;  ///< 缓冲区或请求可用的容量上限，单位遵循所属接口。
  std::vector<char> buffer_;  ///< 对象拥有的字节缓冲区，跨异步挂起保持内容与地址要求。
};

template <class Stream>
  requires borrowed_async_reader<Stream> && borrowed_async_writer<Stream>
class BufStream {
 public:
  explicit BufStream(Stream& stream,
                     std::size_t read_capacity = 8192,
                     std::size_t write_capacity = 8192)
      : reader_(stream, read_capacity), writer_(stream, write_capacity) {}

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  auto read(std::span<char> bytes) { return reader_.read(bytes); }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  auto write(std::span<const char> bytes) { return writer_.write(bytes); }

  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  auto read(io_buffer bytes) { return reader_.read(std::move(bytes)); }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  auto write(io_buffer bytes) { return writer_.write(std::move(bytes)); }

  /// @brief 提供尚未消费的读缓冲区视图，必要时先推进底层读取。
  auto fill_buf() { return reader_.fill_buf(); }

  /// @brief 确认已消费缓冲区前缀，并推进读取位置。
  void consume(std::size_t count) { reader_.consume(count); }

  /// @brief 提交缓冲的写入或已准备的后端请求，失败保留尚未完成的责任。
  auto flush() { return writer_.flush(); }

  /// @brief 启动停止与排空流程，释放已经不再被异步操作借用的服务和句柄。
  auto shutdown() { return writer_.shutdown(); }

 private:
  BufReader<Stream> reader_;  ///< 关联的底层读取或写入对象，所有权由成员声明的值或引用类型决定。
  BufWriter<Stream> writer_;  ///< 关联的底层读取或写入对象，所有权由成员声明的值或引用类型决定。
};

/** @brief 读取到分隔字节（含分隔符）；max_bytes 明确约束单条记录的内存。 */
template <borrowed_async_reader Reader>
task<expected<std::size_t>> read_until(Reader& reader,
                                       char delimiter,
                                       std::vector<char>& output,
                                       std::size_t max_bytes = 1024 * 1024) {
  std::size_t total = 0;
  while (total < max_bytes) {
    if constexpr (requires {
                    reader.fill_buf();
                    reader.consume(std::size_t{});
                  }) {
      auto available = co_await reader.fill_buf();
      if (!available)
        co_return std::unexpected{
            Error{available.error().value(), total, available.error().domain()}};
      if (available->empty())
        co_return total;
      const auto end = std::find(available->begin(), available->end(), delimiter);
      const bool found = end != available->end();
      auto count =
          found ? static_cast<std::size_t>(end - available->begin()) + 1 : available->size();
      count = std::min(count, max_bytes - total);
      output.insert(output.end(),
                    available->begin(),
                    available->begin() + static_cast<std::ptrdiff_t>(count));
      reader.consume(count);
      total += count;
      if (found && count && output.back() == delimiter)
        co_return total;
    } else {
      std::array<char, 1> byte;
      auto count = co_await reader.read(byte);
      if (!count)
        co_return std::unexpected{Error{count.error().value(), total, count.error().domain()}};
      if (!*count)
        co_return total;
      if (*count != 1)
        co_return std::unexpected{Error{EIO, total}};
      output.push_back(byte[0]);
      ++total;
      if (byte[0] == delimiter)
        co_return total;
    }
    co_await this_coro::yield_if_needed();
  }
  co_return std::unexpected{Error{EMSGSIZE, total}};
}

template <borrowed_async_reader Reader>
task<expected<std::size_t>> read_line(Reader& reader,
                                      std::string& output,
                                      std::size_t max_bytes = 1024 * 1024) {
  std::vector<char> bytes;
  auto result = co_await read_until(reader, '\n', bytes, max_bytes);
  output.append(bytes.begin(), bytes.end());
  co_return result;
}

template <borrowed_async_reader Reader>
class Lines {
 public:
  explicit Lines(Reader& reader, std::size_t max_bytes = 1024 * 1024)
      : reader_(reader), max_bytes_(max_bytes) {}

  task<expected<std::optional<std::string>>> next_line() {
    std::string line;
    auto count = co_await read_line(reader_, line, max_bytes_);
    if (!count)
      co_return std::unexpected{count.error()};
    if (!*count)
      co_return std::optional<std::string>{};
    if (line.back() == '\n') {
      line.pop_back();
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
    }
    co_return std::optional<std::string>{std::move(line)};
  }

 private:
  Reader& reader_;  ///< 关联的底层读取或写入对象，所有权由成员声明的值或引用类型决定。
  std::size_t max_bytes_;  ///< 逐段读取允许累计的最大字节数，防止无界增长。
};

template <borrowed_async_reader Reader>
auto lines(Reader& reader, std::size_t max_bytes = 1024 * 1024) {
  return Lines<Reader>{reader, max_bytes};
}

template <borrowed_async_reader Reader>
class SplitByDelimiter {
 public:
  SplitByDelimiter(Reader& reader, char delimiter) : reader_(reader), delimiter_(delimiter) {}

  task<expected<std::optional<std::vector<char>>>> next_segment() {
    std::vector<char> bytes;
    auto count = co_await read_until(reader_, delimiter_, bytes);
    if (!count)
      co_return std::unexpected{count.error()};
    if (!*count)
      co_return std::optional<std::vector<char>>{};
    if (bytes.back() == delimiter_)
      bytes.pop_back();
    co_return std::optional<std::vector<char>>{std::move(bytes)};
  }

 private:
  Reader& reader_;  ///< 关联的底层读取或写入对象，所有权由成员声明的值或引用类型决定。
  char delimiter_;  ///< 按行或分隔符读取时用于识别边界的字节。
};

template <borrowed_async_reader Reader>
auto split_by_delimiter(Reader& reader, char delimiter) {
  return SplitByDelimiter<Reader>{reader, delimiter};
}
}
