#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace faio::io {

/** @brief 独占拥有的 IO 缓冲区。
 * @details capacity 是可写空间，size 是已初始化且可读取的范围。移动把唯一
 * 可变访问权交给操作，异步调用结束以前调用者不能再访问原对象。
 */
class io_buffer {
public:
  explicit io_buffer(std::size_t capacity = 0)
      : storage_(capacity ? std::make_unique_for_overwrite<char[]>(capacity)
                          : nullptr),
        capacity_(capacity) {}
  io_buffer(const io_buffer &) = delete;
  io_buffer &operator=(const io_buffer &) = delete;
  io_buffer(io_buffer &&other) noexcept
      : storage_(std::move(other.storage_)),
        capacity_(std::exchange(other.capacity_, 0)),
        size_(std::exchange(other.size_, 0)) {}
  io_buffer &operator=(io_buffer &&other) noexcept {
    if (this != &other) {
      storage_ = std::move(other.storage_);
      capacity_ = std::exchange(other.capacity_, 0);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }
  ~io_buffer() = default;

  /** @brief 复制输入字节，返回可用于异步写入的独占缓冲区。 */
  static io_buffer copy(std::span<const char> bytes) {
    io_buffer result(bytes.size());
    if (!bytes.empty())
      std::memcpy(result.data(), bytes.data(), bytes.size());
    result.size_ = bytes.size();
    return result;
  }
  static io_buffer copy(std::string_view bytes) {
    return copy(std::span<const char>{bytes.data(), bytes.size()});
  }
  [[nodiscard]] char *data() noexcept { return storage_.get(); }
  [[nodiscard]] const char *data() const noexcept { return storage_.get(); }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  /** @brief 读取已初始化的字节，不暴露尚未写入的尾部内存。 */
  [[nodiscard]] std::span<const char> bytes() const noexcept {
    return {data(), size_};
  }
  [[nodiscard]] std::span<char> bytes() noexcept { return {data(), size_}; }
  [[nodiscard]] std::span<const char> span() const noexcept { return bytes(); }
  /** @brief 获取全部写入空间；写入后必须调用 set_size 发布初始化范围。 */
  [[nodiscard]] std::span<char> writable_bytes() noexcept {
    return {data(), capacity_};
  }
  [[nodiscard]] std::span<char> mutable_bytes() noexcept {
    return writable_bytes();
  }
  /** @brief 标记已经初始化的范围；不能声明超过 capacity 的字节已初始化。 */
  void set_size(std::size_t initialized) {
    if (initialized > capacity_)
      throw std::out_of_range("io_buffer 初始化范围超过容量");
    size_ = initialized;
  }
  /** @brief 调整已初始化长度；增长部分显式清零，避免读取未初始化内存。 */
  void resize(std::size_t size) {
    if (size > capacity_)
      reserve(size);
    if (size > size_)
      std::memset(data() + size_, 0, size - size_);
    size_ = size;
  }
  /** @brief 扩大容量，只复制已初始化的有效数据。 */
  void reserve(std::size_t capacity) {
    if (capacity <= capacity_)
      return;
    auto replacement = std::make_unique_for_overwrite<char[]>(capacity);
    if (size_)
      std::memcpy(replacement.get(), data(), size_);
    storage_ = std::move(replacement);
    capacity_ = capacity;
  }

private:
  std::unique_ptr<char[]> storage_;
  std::size_t capacity_{};
  std::size_t size_{};
};

/** @brief 显式可变借用；调用者保证操作排空前内存有效且没有并发访问。 */
struct borrowed_buffer {
  std::span<char> bytes;
  explicit borrowed_buffer(std::span<char> view) noexcept : bytes(view) {}
};
/** @brief 显式只读借用；调用者保证操作排空前内存有效且没有写入。 */
struct borrowed_const_buffer {
  std::span<const char> bytes;
  explicit borrowed_const_buffer(std::span<const char> view) noexcept
      : bytes(view) {}
};

/** @brief 多个发送操作可共享的不可变字节拥有者。 */
class shared_const_buffer {
public:
  explicit shared_const_buffer(std::span<const char> bytes)
      : storage_(std::make_shared<const std::vector<char>>(bytes.begin(),
                                                           bytes.end())) {}
  explicit shared_const_buffer(std::string_view bytes)
      : shared_const_buffer(std::span<const char>{bytes.data(), bytes.size()}) {
  }
  [[nodiscard]] std::span<const char> bytes() const noexcept {
    return *storage_;
  }
  [[nodiscard]] std::size_t size() const noexcept { return storage_->size(); }

private:
  std::shared_ptr<const std::vector<char>> storage_;
};

/** @brief 拥有型操作完成后返还缓冲区和本次实际传输量。 */
struct io_transfer {
  io_buffer buffer;
  std::size_t bytes{};
};

/** @brief 已初始化范围与剩余容量的显式读缓冲视图。 */
class read_buf {
public:
  explicit read_buf(std::span<char> storage, std::size_t initialized = 0)
      : storage_(storage), initialized_(initialized) {
    if (initialized > storage.size())
      throw std::out_of_range("read_buf 初始化范围超过容量");
  }
  [[nodiscard]] std::span<const char> filled() const noexcept {
    return storage_.first(initialized_);
  }
  [[nodiscard]] std::span<char> unfilled() noexcept {
    return storage_.subspan(initialized_);
  }
  [[nodiscard]] std::size_t size() const noexcept { return initialized_; }
  [[nodiscard]] std::size_t capacity() const noexcept {
    return storage_.size();
  }
  void advance(std::size_t bytes) {
    if (bytes > storage_.size() - initialized_)
      throw std::out_of_range("read_buf 前进量超过剩余容量");
    initialized_ += bytes;
  }
  void clear() noexcept { initialized_ = 0; }

private:
  std::span<char> storage_;
  std::size_t initialized_{};
};
} // namespace faio::io
