#pragma once
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/io.hpp"
#include <concepts>
#include <sys/stat.h>

#ifdef unix
#undef unix
#endif
namespace faio::io::unix {
/** @brief POSIX描述符的移动独占所有者，析构只close一次，不重试复用的整数fd。 */
class OwnedFd {
public:
  explicit OwnedFd(int descriptor = -1) noexcept : descriptor_(descriptor) {}
  OwnedFd(const OwnedFd &) = delete;
  OwnedFd &operator=(const OwnedFd &) = delete;
  OwnedFd(OwnedFd &&other) noexcept : descriptor_(other.release()) {}
  OwnedFd &operator=(OwnedFd &&other) noexcept {
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
  [[nodiscard]] int get() const noexcept { return descriptor_; }
  int release() noexcept { return std::exchange(descriptor_, -1); }

private:
  int descriptor_;
};
/** @brief
 * 可交接描述符的原生拥有者；重新构造必须不抛出，以免导出后丢失关闭责任。 */
template <class Owner>
concept native_fd_owner =
    std::is_nothrow_constructible_v<Owner, int> && requires(Owner &owner) {
      { owner.get() } -> std::same_as<int>;
      { owner.release() } noexcept -> std::same_as<int>;
    };

/** @brief 一次就绪观察的代际guard；假就绪仍需真实非阻塞syscall确认。 */
class AsyncFdReadyGuard {
public:
  AsyncFdReadyGuard(detail::resource_ptr resource, Interest interest,
                    Ready ready, std::uint64_t generation)
      : resource_(std::move(resource)), interest_(interest), ready_(ready),
        generation_(generation) {}
  [[nodiscard]] Ready ready() const noexcept { return ready_; }
  /** @brief 只清除本次观察代际；更新的内核事件不会被旧guard抹除。 */
  void clear_ready() noexcept {
    if (resource_->owner)
      resource_->owner->clear_readiness(resource_, interest_, generation_);
  }
  /** @brief callback只可执行同方向非阻塞调用；EAGAIN原子清除对应readiness。 */
  template <class F> auto try_io(F &&function) -> std::invoke_result_t<F, int> {
    return detail::with_resource(resource_, interest_, [&] {
      return std::invoke(std::forward<F>(function), resource_->fd());
    });
  }

private:
  detail::resource_ptr resource_;
  Interest interest_;
  Ready ready_;
  std::uint64_t generation_;
};

namespace detail_fd {
inline int prepare_fd(int descriptor) {
  struct stat attributes;
  if (::fstat(descriptor, &attributes))
    throw std::system_error(errno, std::generic_category());
  if (S_ISREG(attributes.st_mode) || S_ISDIR(attributes.st_mode) ||
      S_ISBLK(attributes.st_mode))
    throw std::system_error(EINVAL, std::generic_category(),
                            "AsyncFd不支持普通磁盘文件，请使用fs::File");
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0 ||
      ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0)
    throw std::system_error(errno, std::generic_category());
  return descriptor;
}
inline task<expected<AsyncFdReadyGuard>> observe(detail::resource_ptr resource,
                                                 Interest interest) {
  auto observed = co_await io::ready(resource, interest);
  if (!observed)
    co_return std::unexpected{observed.error()};
  co_return AsyncFdReadyGuard{std::move(resource), interest, *observed,
                              observed->generation};
}
} // namespace detail_fd
/** @brief 泛型可非阻塞描述符的就绪注册；不把普通文件readiness解释成异步磁盘IO。
 * @details 资源长期归属一个IO domain，协程移动不会改变注册。实际IO由调用者
 * 的非阻塞syscall执行，借用内存必须覆盖调用。guard不拥有数据方向执行权。
 */
template <native_fd_owner Owner = OwnedFd>
class AsyncFd : public detail::FileDescriptor {
public:
  explicit AsyncFd(Owner owner, io_context context = io_context::current())
      : FileDescriptor(detail_fd::prepare_fd(owner.get()), std::move(context)) {
    (void)owner.release();
  }
  AsyncFd(AsyncFd &&) noexcept = default;
  AsyncFd &operator=(AsyncFd &&) noexcept = default;
  static expected<AsyncFd> create(io_context context, Owner owner) {
    try {
      return AsyncFd{std::move(owner), std::move(context)};
    } catch (const std::system_error &error) {
      return std::unexpected{make_error(error.code().value())};
    } catch (const std::bad_alloc &) {
      return std::unexpected{make_error(ENOMEM)};
    }
  }
  auto ready(Interest interest) {
    return detail_fd::observe(resource(), interest);
  }
  auto readable() { return ready(Interest::readable); }
  auto writable() { return ready(Interest::writable); }
  [[nodiscard]] int native_handle() const noexcept { return fd(); }
  template <class F>
  auto try_io(Interest interest, F &&function) -> std::invoke_result_t<F, int> {
    return detail::with_resource(resource(), interest, [&] {
      return std::invoke(std::forward<F>(function), fd());
    });
  }
  expected<Owner> into_inner() {
    auto descriptor = into_native();
    if (!descriptor)
      return std::unexpected{descriptor.error()};
    return Owner{*descriptor};
  }
};
} // namespace faio::io::unix
