/**
 * @file faio/detail/io/core/file_descriptor.hpp
 * @brief 跨平台异步 IO：原生描述符所有权。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/io/awaiter/close.hpp"
#include "faio/detail/io/platform/native_operations.hpp"

namespace faio::io::detail {

/** @brief 资源包装所有者；移动/共享不改变 IO 归属，析构只请求非阻塞关闭。 */
class FileDescriptor {
 protected:
  explicit FileDescriptor(native_descriptor fd,
                          io_context context = io_context::current(),
                          bool regular = false)
      : resource_(adopt_resource(context, fd, true, regular)) {
    resource_->wrappers.fetch_add(1, std::memory_order_relaxed);
  }

  explicit FileDescriptor(io_context context, native_descriptor fd, bool regular = false)
      : FileDescriptor(fd, std::move(context), regular) {}

  explicit FileDescriptor(resource_ptr resource) : resource_(std::move(resource)) {
    if (resource_)
      resource_->wrappers.fetch_add(1, std::memory_order_relaxed);
  }

  ~FileDescriptor() { release(); }

  FileDescriptor(FileDescriptor&& other) noexcept : resource_(std::move(other.resource_)) {}

  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      release();
      resource_ = std::move(other.resource_);
    }
    return *this;
  }

  FileDescriptor(const FileDescriptor&) = delete;

  FileDescriptor& operator=(const FileDescriptor&) = delete;

 public:
  /// @brief 读取原生句柄值，不在此接口转移关闭责任。
  native_descriptor fd() const noexcept { return resource_ ? resource_->fd() : -1; }

  /// @brief 取得底层共享资源控制块，以保持句柄身份与所属域。
  const resource_ptr& resource() const noexcept { return resource_; }

  /// @brief 取得当前对象关联的 IO 上下文租约。
  io_context context() const noexcept {
    return resource_ ? io_context{resource_->owner} : io_context{};
  }

  /// @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。
  auto close() noexcept { return Close{resource_}; }

  /// @brief 把原生句柄及关闭责任转出，源包装随后不再拥有该句柄。
  expected<native_descriptor> into_native() noexcept {
    if (!resource_)
      return std::unexpected{make_error(EBADF)};
    if (resource_->owner)
      return resource_->owner->detach(*resource_);
    const native_descriptor value = resource_->handle.exchange(-1);
    resource_->owns_handle = false;
    if (value < 0)
      return std::unexpected{make_error(EBADF)};
    return value;
  }

  native_descriptor take_fd() noexcept {
    auto value = into_native();
    return value ? *value : -1;
  }

  /** @brief 设置异步句柄为非阻塞；关闭非阻塞模式会返回 EINVAL。
   * @details 后端直接尝试系统调用，O_NONBLOCK 是保障 worker
   * 不被内核阻塞的前提。 如需原生阻塞句柄，应先通过 into_native
   * 导出所有权再修改 flags。
   */
  expected<void> set_nonblocking(bool enabled) const noexcept {
    return with_resource(resource_, Interest::none, [&] {
      return native_operations::set_nonblocking(fd(), enabled);
    });
  }

  /// @brief 配置原生描述符的非阻塞状态，错误按平台原生约定转换。
  expected<bool> nonblocking() const noexcept {
    return with_resource(
        resource_, Interest::none, [&] { return native_operations::nonblocking(fd()); });
  }

 private:
  /// @brief 转出或归还当前类型约定的资源责任，并把源对象变为空状态。
  void release() noexcept {
    if (!resource_)
      return;
    if (resource_->wrappers.fetch_sub(1, std::memory_order_acq_rel) == 1 && resource_->owner)
      resource_->owner->request_close(resource_);
    resource_.reset();
  }

  resource_ptr resource_;  ///< 共享资源控制块租约，跨挂起固定句柄身份及 IO 域归属。
};

}
