/**
 * @file faio/detail/io/platform/unix/async_fd.hpp
 * @brief 跨平台异步 IO：异步描述符就绪守卫。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#if !defined(_WIN32)
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/awaiter/operations.hpp"
#include "faio/detail/io/core/file_descriptor.hpp"
#include "faio/detail/io/platform/unix/owned_fd.hpp"
#include "faio/detail/io/traits/native_fd_owner.hpp"
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <new>
#include <sys/stat.h>
#include <system_error>
#include <type_traits>
#include <utility>

namespace faio::io::unix {
/** @brief 一次就绪观察的代际guard；假就绪仍需真实非阻塞syscall确认。 */
class AsyncFdReadyGuard {
 public:
  AsyncFdReadyGuard(detail::resource_ptr resource,
                    Interest interest,
                    Ready ready,
                    std::uint64_t generation)
      : resource_(std::move(resource)),
        interest_(interest),
        ready_(ready),
        generation_(generation) {}

  [[nodiscard]] Ready ready() const noexcept { return ready_; }

  /** @brief 只清除本次观察代际；更新的内核事件不会被旧guard抹除。 */
  void clear_ready() noexcept {
    if (resource_->owner)
      resource_->owner->clear_readiness(resource_, interest_, generation_);
  }

  /** @brief callback只可执行同方向非阻塞调用；EAGAIN原子清除对应readiness。 */
  template <class F>
  auto try_io(F&& function) -> std::invoke_result_t<F, int> {
    return detail::with_resource(resource_, interest_, [&] {
      return std::invoke(std::forward<F>(function), resource_->fd());
    });
  }

 private:
  detail::resource_ptr resource_;  ///< 共享资源控制块租约，跨挂起固定句柄身份及 IO 域归属。
  Interest interest_;  ///< 当前等待或预留关注的读写方向集合。
  Ready ready_;  ///< 待交付的就绪结果或就绪条目链表头。
  std::uint64_t generation_;  ///< 获取就绪提示时的资源代际，清除旧提示时不得覆盖新事件。
};

namespace detail_fd {
inline int prepare_fd(int descriptor) {
  struct stat attributes;
  if (::fstat(descriptor, &attributes))
    throw std::system_error(errno, std::generic_category());
  if (S_ISREG(attributes.st_mode) || S_ISDIR(attributes.st_mode) || S_ISBLK(attributes.st_mode))
    throw std::system_error(
        EINVAL, std::generic_category(), "AsyncFd不支持普通磁盘文件，请使用fs::File");
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0
      || ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0)
    throw std::system_error(errno, std::generic_category());
  return descriptor;
}

inline task<expected<AsyncFdReadyGuard>> observe(detail::resource_ptr resource, Interest interest) {
  auto observed = co_await io::ready(resource, interest);
  if (!observed)
    co_return std::unexpected{observed.error()};
  co_return AsyncFdReadyGuard{std::move(resource), interest, *observed, observed->generation};
}
}

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

  AsyncFd(AsyncFd&&) noexcept = default;

  AsyncFd& operator=(AsyncFd&&) noexcept = default;

  static expected<AsyncFd> create(io_context context, Owner owner) {
    try {
      return AsyncFd{std::move(owner), std::move(context)};
    } catch (const std::system_error& error) {
      return std::unexpected{make_error(error.code().value())};
    } catch (const std::bad_alloc&) {
      return std::unexpected{make_error(ENOMEM)};
    }
  }

  auto ready(Interest interest) { return detail_fd::observe(resource(), interest); }

  auto readable() { return ready(Interest::readable); }

  auto writable() { return ready(Interest::writable); }

  /// @brief 读取原生句柄值，不在此接口转移关闭责任。
  [[nodiscard]] int native_handle() const noexcept { return fd(); }

  template <class F>
  auto try_io(Interest interest, F&& function) -> std::invoke_result_t<F, int> {
    return detail::with_resource(
        resource(), interest, [&] { return std::invoke(std::forward<F>(function), fd()); });
  }

  expected<Owner> into_inner() {
    auto descriptor = into_native();
    if (!descriptor)
      return std::unexpected{descriptor.error()};
    return Owner{*descriptor};
  }
};
}
#endif
