/**
 * @file faio/detail/io/platform/unix/owned_fd.hpp
 * @brief 描述符 RAII 所有者。
 */
#pragma once
#if !defined(_WIN32)
#include <unistd.h>
#include <utility>

#ifdef unix
#undef unix
#endif
namespace faio::io::unix {
/**  @brief 可移动的 POSIX 描述符独占所有者；析构仅关闭一次，避免重试时误关已复用的 fd。 */
class owned_fd {
 public:
  /** @brief 构造独占原生描述符。 */
  explicit owned_fd(int descriptor = -1) noexcept : descriptor_(descriptor) {}

  /** @brief 禁止复制构造独占原生描述符。 */
  owned_fd(const owned_fd&) = delete;

  /** @brief 禁止复制赋值。 */
  owned_fd& operator=(const owned_fd&) = delete;

  /** @brief 移动构造独占原生描述符。 */
  owned_fd(owned_fd&& other) noexcept : descriptor_(other.release()) {}

  /** @brief 移动赋值。 */
  owned_fd& operator=(owned_fd&& other) noexcept {
    if (this != &other) {
      if (descriptor_ >= 0)
        ::close(descriptor_);
      descriptor_ = other.release();
    }
    return *this;
  }

  /** @brief 销毁独占原生描述符。 */
  ~owned_fd() {
    if (descriptor_ >= 0)
      ::close(descriptor_);
  }

  /** @brief 借用持有的 POSIX 描述符。 */
  [[nodiscard]] int get() const noexcept { return descriptor_; }

  /** @brief 转出描述符及关闭责任，并将源对象置空。 */
  int release() noexcept { return std::exchange(descriptor_, -1); }

 private:
  int descriptor_; /**< 独占拥有的 POSIX 描述符，转出后置为无效值。 */
};

}  // namespace faio::io::unix
#endif
