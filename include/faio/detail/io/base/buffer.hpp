/**
 * @file faio/detail/io/base/buffer.hpp
 * @brief 字节缓冲区与所有权。
 */
#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace faio::io {
/**
 *  @brief 独占持有的 IO 缓冲区。
 * @details capacity 表示可写容量，size 表示已初始化且可读取的字节数。
 * 移动后，操作独占可变访问权；异步调用结束前，调用者不能再访问原对象。
 */
class io_buffer {
 public:
  /** @brief 构造独占的 I/O 字节缓冲区。 */
  explicit io_buffer(std::size_t capacity = 0)
      : storage_(capacity ? std::make_unique_for_overwrite<char[]>(capacity) : nullptr),
        capacity_(capacity) {}

  /** @brief 禁止复制构造独占的 I/O 字节缓冲区。 */
  io_buffer(const io_buffer&) = delete;

  /** @brief 禁止复制赋值。 */
  io_buffer& operator=(const io_buffer&) = delete;

  /** @brief 移动构造独占的 I/O 字节缓冲区。 */
  io_buffer(io_buffer&& other) noexcept
      : storage_(std::move(other.storage_)),
        capacity_(std::exchange(other.capacity_, 0)),
        size_(std::exchange(other.size_, 0)) {}

  /** @brief 移动赋值。 */
  io_buffer& operator=(io_buffer&& other) noexcept {
    if (this != &other) {
      storage_ = std::move(other.storage_);
      capacity_ = std::exchange(other.capacity_, 0);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }

  /** @brief 销毁独占的 I/O 字节缓冲区。 */
  ~io_buffer() = default;

  /**  @brief 复制输入字节，返回可用于异步写入的独占缓冲区。 */
  static io_buffer copy(std::span<const char> bytes) {
    io_buffer result(bytes.size());
    if (!bytes.empty())
      std::memcpy(result.data(), bytes.data(), bytes.size());
    result.size_ = bytes.size();
    return result;
  }

  /** @brief 复制字符串中的字节，返回可用于异步写入的独占缓冲区。 */
  static io_buffer copy(std::string_view bytes) {
    return copy(std::span<const char>{bytes.data(), bytes.size()});
  }

  /** @brief 借用底层可写字节存储，指针不延长缓冲区的有效期。 */
  [[nodiscard]] char* data() noexcept { return storage_.get(); }

  /** @brief 借用底层只读字节存储，指针不延长缓冲区的有效期。 */
  [[nodiscard]] const char* data() const noexcept { return storage_.get(); }

  /** @brief 返回已初始化的字节数。 */
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  /** @brief 返回缓冲区的字节容量。 */
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  /** @brief 判断缓冲区是否没有已初始化的字节。 */
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  /**  @brief 读取已初始化的字节，不暴露尚未写入的尾部内存。 */
  [[nodiscard]] std::span<const char> bytes() const noexcept { return {data(), size_}; }

  /** @brief 借用已初始化的可写字节范围，视图不延长缓冲区的有效期。 */
  [[nodiscard]] std::span<char> bytes() noexcept { return {data(), size_}; }

  /** @brief 借用已初始化的只读字节范围，视图不延长缓冲区的有效期。 */
  [[nodiscard]] std::span<const char> span() const noexcept { return bytes(); }

  /**  @brief 获取全部写入空间；写入后必须调用 set_size 发布初始化范围。 */
  [[nodiscard]] std::span<char> writable_bytes() noexcept { return {data(), capacity_}; }

  /** @brief 取得可写字节视图。 */
  [[nodiscard]] std::span<char> mutable_bytes() noexcept { return writable_bytes(); }

  /**  @brief 标记已经初始化的范围；不能声明超过 capacity 的字节已初始化。 */
  void set_size(std::size_t initialized) {
    if (initialized > capacity_)
      throw std::out_of_range("io_buffer 初始化范围超过容量");
    size_ = initialized;
  }

  /**  @brief 调整已初始化长度；增长部分显式清零，避免读取未初始化内存。 */
  void resize(std::size_t size) {
    if (size > capacity_)
      reserve(size);
    if (size > size_)
      std::memset(data() + size_, 0, size - size_);
    size_ = size;
  }

  /**  @brief 扩大容量，只复制已初始化的有效数据。 */
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
  std::unique_ptr<char[]> storage_; /**< 独占持有的字节存储。 */
  std::size_t capacity_{};          /**< 缓冲区的字节容量。 */
  std::size_t size_{};              /**< 已初始化的字节数。 */
};

/**  @brief 显式借用可写缓冲区；调用者须保证操作排空前内存有效，且没有并发访问。 */
struct borrowed_buffer {
  std::span<char> bytes; /**< 借用的可写字节区，操作完成前须保持有效。 */

  /** @brief 从可写字节视图构造借用缓冲区。 */
  explicit borrowed_buffer(std::span<char> view) noexcept : bytes(view) {}
};

/**  @brief 显式借用只读缓冲区；调用者须保证操作排空前内存有效，且没有写入。 */
struct borrowed_const_buffer {
  std::span<const char> bytes; /**< 发送时借用的只读字节区，完成前保持有效。 */

  /** @brief 从只读字节视图构造借用缓冲区。 */
  explicit borrowed_const_buffer(std::span<const char> view) noexcept : bytes(view) {}
};

/**  @brief 可供多个发送操作共享的不可变字节存储。 */
class shared_const_buffer {
 public:
  /** @brief 构造共享只读字节缓冲区。 */
  explicit shared_const_buffer(std::span<const char> bytes)
      : storage_(std::make_shared<const std::vector<char>>(bytes.begin(), bytes.end())) {}

  /** @brief 构造共享只读字节缓冲区。 */
  explicit shared_const_buffer(std::string_view bytes)
      : shared_const_buffer(std::span<const char>{bytes.data(), bytes.size()}) {}

  /** @brief 返回内部字节存储的视图；视图不延长对象或负载的生命周期。 */
  [[nodiscard]] std::span<const char> bytes() const noexcept { return *storage_; }

  /** @brief 返回已初始化的字节数。 */
  [[nodiscard]] std::size_t size() const noexcept { return storage_->size(); }

 private:
  std::shared_ptr<const std::vector<char>> storage_; /**< 多个发送操作共享持有的不可变字节存储。 */
};

/**  @brief 持有缓冲区所有权的操作完成后，返还缓冲区和本次实际传输量。 */
struct io_transfer {
  io_buffer buffer;    /**< 对象持有的字节缓冲区，在挂起期间保持所需内容和地址有效。 */
  std::size_t bytes{}; /**< 本次操作实际传输的字节数。 */
};

/**  @brief 显式表示已初始化范围与剩余容量的读缓冲视图。 */
class read_buf {
 public:
  /** @brief 构造借用读缓冲视图。 */
  explicit read_buf(std::span<char> storage, std::size_t initialized = 0)
      : storage_(storage), initialized_(initialized) {
    if (initialized > storage.size())
      throw std::out_of_range("read_buf 初始化范围超过容量");
  }

  /** @brief 取得已初始化字节视图。 */
  [[nodiscard]] std::span<const char> filled() const noexcept {
    return storage_.first(initialized_);
  }

  /** @brief 取得尚未初始化的可写范围。 */
  [[nodiscard]] std::span<char> unfilled() noexcept { return storage_.subspan(initialized_); }

  /** @brief 返回已初始化的字节数。 */
  [[nodiscard]] std::size_t size() const noexcept { return initialized_; }

  /** @brief 返回缓冲区的字节容量。 */
  [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

  /** @brief 将已初始化范围向后扩展指定字节数。 */
  void advance(std::size_t bytes) {
    if (bytes > storage_.size() - initialized_)
      throw std::out_of_range("read_buf 前进量超过剩余容量");
    initialized_ += bytes;
  }

  /** @brief 将已初始化的字节数清零。 */
  void clear() noexcept { initialized_ = 0; }

 private:
  std::span<char> storage_;   /**< 由调用方保证有效的借用字节存储。 */
  std::size_t initialized_{}; /**< 已完成初始化、允许安全读取的缓冲字节数。 */
};
}  // namespace faio::io
