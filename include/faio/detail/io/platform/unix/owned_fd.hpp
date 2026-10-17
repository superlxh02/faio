/**
 * @file faio/detail/io/platform/unix/owned_fd.hpp
 * @brief 跨平台异步 IO：描述符 RAII 所有者。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#if !defined(_WIN32)
#include <unistd.h>
#include <utility>

#ifdef unix
#undef unix
#endif
namespace faio::io::unix {
/** @brief POSIX描述符的移动独占所有者，析构只close一次，不重试复用的整数fd。 */
class OwnedFd {
 public:
  explicit OwnedFd(int descriptor = -1) noexcept : descriptor_(descriptor) {}

  OwnedFd(const OwnedFd&) = delete;

  OwnedFd& operator=(const OwnedFd&) = delete;

  OwnedFd(OwnedFd&& other) noexcept : descriptor_(other.release()) {}

  OwnedFd& operator=(OwnedFd&& other) noexcept {
    if (this != &other) {
      if (descriptor_ >= 0)
        ::close(descriptor_);
      descriptor_ = other.release();
    }
    return *this;
  }

  ~OwnedFd() {
    if (descriptor_ >= 0)
      ::close(descriptor_);
  }

  /// @brief 取得当前对象保存的值或借用句柄，是否转交所有权由返回类型决定。
  [[nodiscard]] int get() const noexcept { return descriptor_; }

  /// @brief 转出或归还当前类型约定的资源责任，并把源对象变为空状态。
  int release() noexcept { return std::exchange(descriptor_, -1); }

 private:
  int descriptor_;  ///< 独占拥有的 POSIX 描述符，转出后置为无效值。
};

}
#endif
