/**
 * @file faio/detail/io/core/file_descriptor.hpp
 * @brief 原生描述符所有权。
 */
#pragma once
#include "faio/detail/io/awaiter/close.hpp"
#include "faio/detail/io/platform/native_operations.hpp"

namespace faio::io::detail {

/**  @brief 持有资源所有权的包装对象；移动/共享不改变 IO 归属，析构只请求非阻塞关闭。 */
class file_descriptor {
 protected:
  /** @brief 构造持有所有权的文件描述符。 */
  explicit file_descriptor(native_descriptor fd,
                           io_context context = io_context::current(),
                           bool regular = false)
      : resource_(adopt_resource(context, fd, true, regular)) {
    resource_->wrappers.fetch_add(1, std::memory_order_relaxed);
  }

  /** @brief 构造持有所有权的文件描述符。 */
  explicit file_descriptor(io_context context, native_descriptor fd, bool regular = false)
      : file_descriptor(fd, std::move(context), regular) {}

  /** @brief 构造持有所有权的文件描述符。 */
  explicit file_descriptor(resource_ptr resource) : resource_(std::move(resource)) {
    if (resource_)
      resource_->wrappers.fetch_add(1, std::memory_order_relaxed);
  }

  /** @brief 销毁持有所有权的文件描述符。 */
  ~file_descriptor() { release(); }

  /** @brief 移动构造持有所有权的文件描述符。 */
  file_descriptor(file_descriptor&& other) noexcept : resource_(std::move(other.resource_)) {}

  /** @brief 移动赋值。 */
  file_descriptor& operator=(file_descriptor&& other) noexcept {
    if (this != &other) {
      release();
      resource_ = std::move(other.resource_);
    }
    return *this;
  }

  /** @brief 禁止复制构造持有所有权的文件描述符。 */
  file_descriptor(const file_descriptor&) = delete;

  /** @brief 禁止复制赋值。 */
  file_descriptor& operator=(const file_descriptor&) = delete;

 public:
  /** @brief 读取原生句柄值，不转移关闭责任。 */
  native_descriptor fd() const noexcept { return resource_ ? resource_->fd() : -1; }

  /** @brief 取得底层共享资源控制块，保持句柄身份及所属域有效。 */
  const resource_ptr& resource() const noexcept { return resource_; }

  /** @brief 取得该对象关联的 I/O 上下文租约。 */
  io_context context() const noexcept {
    return resource_ ? io_context{resource_->owner} : io_context{};
  }

  /** @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。 */
  auto close() noexcept { return close_awaiter{resource_}; }

  /** @brief 把原生句柄及关闭责任转出，源包装随后不再拥有该句柄。 */
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

  /** @brief 转出原生描述符及其关闭责任。 */
  native_descriptor take_fd() noexcept {
    auto value = into_native();
    return value ? *value : -1;
  }

  /**
   *  @brief 设置异步句柄为非阻塞；关闭非阻塞模式会返回 EINVAL。
   * @details 后端直接尝试系统调用，O_NONBLOCK 是保障 worker
   * 不被内核阻塞的前提。 如需原生阻塞句柄，应先通过 into_native
   * 导出所有权再修改 flags。
   */
  expected<void> set_nonblocking(bool enabled) const noexcept {
    return with_resource(resource_, interest::NONE, [&] {
      return native_operations::set_nonblocking(fd(), enabled);
    });
  }

  /** @brief 查询原生描述符的非阻塞状态，错误按平台原生约定转换。 */
  expected<bool> nonblocking() const noexcept {
    return with_resource(
        resource_, interest::NONE, [&] { return native_operations::nonblocking(fd()); });
  }

 private:
  /** @brief 按本类型的约定转出或归还资源责任，并将源对象置空。 */
  void release() noexcept {
    if (!resource_)
      return;
    if (resource_->wrappers.fetch_sub(1, std::memory_order_acq_rel) == 1 && resource_->owner)
      resource_->owner->request_close(resource_);
    resource_.reset();
  }

  resource_ptr resource_; /**< 共享资源控制块的租约，在挂起期间保持句柄身份及所属 I/O 域不变。 */
};

}  // namespace faio::io::detail
