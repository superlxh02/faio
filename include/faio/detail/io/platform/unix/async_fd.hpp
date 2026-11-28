/**
 * @file faio/detail/io/platform/unix/async_fd.hpp
 * @brief 异步描述符就绪守卫。
 */
#pragma once
#if !defined(_WIN32)
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/awaiter/ready.hpp"
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
/**  @brief 按代际保护一次就绪观察；假就绪仍须通过真实的非阻塞系统调用确认。 */
class async_fd_ready_guard {
 public:
  /** @brief 构造描述符就绪状态保护。 */
  async_fd_ready_guard(detail::resource_ptr resource,
                       interest interest,
                       readiness ready,
                       std::uint64_t generation)
      : resource_(std::move(resource)),
        interest_(interest),
        ready_(ready),
        generation_(generation) {}

  /** @brief 检查资源或操作的就绪状态。 */
  [[nodiscard]] readiness ready() const noexcept { return ready_; }

  /**  @brief 仅清除本次观察代际的就绪提示，保留后来到达的内核事件。 */
  void clear_ready() noexcept {
    if (resource_->owner)
      resource_->owner->clear_readiness(resource_, interest_, generation_);
  }

  /**  @brief 回调只能执行同方向的非阻塞调用；EAGAIN 会原子地清除对应就绪提示。 */
  template <class F>
  auto try_io(F&& function) -> std::invoke_result_t<F, int> {
    return detail::with_resource(resource_, interest_, [&] {
      return std::invoke(std::forward<F>(function), resource_->fd());
    });
  }

 private:
  detail::resource_ptr
      resource_;             /**< 共享资源控制块的租约，在挂起期间保持句柄身份及所属 I/O 域不变。 */
  interest interest_;        /**< 当前等待或预留关注的读写方向集合。 */
  readiness ready_;          /**< 本次观察到的就绪结果。 */
  std::uint64_t generation_; /**< 获取就绪提示时的资源代际，清除旧提示时不得覆盖新事件。 */
};

namespace detail_fd {
/** @brief 设置原生描述符的异步访问标志。 */
inline int prepare_fd(int descriptor) {
  struct stat attributes;
  if (::fstat(descriptor, &attributes))
    throw std::system_error(errno, std::generic_category());
  if (S_ISREG(attributes.st_mode) || S_ISDIR(attributes.st_mode) || S_ISBLK(attributes.st_mode))
    throw std::system_error(
        EINVAL, std::generic_category(), "async_fd 不支持普通磁盘文件，请使用fs::file");
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0
      || ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0)
    throw std::system_error(errno, std::generic_category());
  return descriptor;
}

/** @brief 观察已发布的完成状态。 */
inline task<expected<async_fd_ready_guard>> observe(detail::resource_ptr resource,
                                                    interest interest) {
  auto observed = co_await io::ready(resource, interest);
  if (!observed)
    co_return std::unexpected{observed.error()};
  co_return async_fd_ready_guard{std::move(resource), interest, *observed, observed->generation};
}
}  // namespace detail_fd

/**
 *  @brief 为可设为非阻塞的描述符注册就绪观察；普通文件就绪不代表异步磁盘 I/O。
 * @details 资源长期归属同一 I/O 域，协程移动不改变注册。实际 I/O 由调用者执行
 * 非阻塞系统调用，借用内存须在调用期间保持有效。就绪守卫不持有数据方向执行权。
 */
template <native_fd_owner Owner = owned_fd>
class async_fd : public detail::file_descriptor {
 public:
  /** @brief 构造异步原生描述符。 */
  explicit async_fd(Owner owner, io_context context = io_context::current())
      : file_descriptor(detail_fd::prepare_fd(owner.get()), std::move(context)) {
    (void)owner.release();
  }

  /** @brief 移动构造异步原生描述符。 */
  async_fd(async_fd&&) noexcept = default;

  /** @brief 移动赋值。 */
  async_fd& operator=(async_fd&&) noexcept = default;

  /** @brief 注册原生描述符并创建异步访问封装。 */
  static expected<async_fd> create(io_context context, Owner owner) {
    try {
      return async_fd{std::move(owner), std::move(context)};
    } catch (const std::system_error& error) {
      return std::unexpected{make_error(error.code().value())};
    } catch (const std::bad_alloc&) {
      return std::unexpected{make_error(ENOMEM)};
    }
  }

  /** @brief 检查资源或操作的就绪状态。 */
  auto ready(interest interest) { return detail_fd::observe(resource(), interest); }

  /** @brief 等待或查询资源可读状态。 */
  auto readable() { return ready(interest::READABLE); }

  /** @brief 等待或查询资源可写状态。 */
  auto writable() { return ready(interest::WRITABLE); }

  /** @brief 读取原生句柄值，不转移关闭责任。 */
  [[nodiscard]] int native_handle() const noexcept { return fd(); }

  /** @brief 尝试立即完成 I/O 操作。 */
  template <class F>
  auto try_io(interest interest, F&& function) -> std::invoke_result_t<F, int> {
    return detail::with_resource(
        resource(), interest, [&] { return std::invoke(std::forward<F>(function), fd()); });
  }

  /** @brief 取回底层流对象。 */
  expected<Owner> into_inner() {
    auto descriptor = into_native();
    if (!descriptor)
      return std::unexpected{descriptor.error()};
    return Owner{*descriptor};
  }
};
}  // namespace faio::io::unix
#endif
