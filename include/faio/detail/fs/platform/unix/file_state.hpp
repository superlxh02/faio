/**
 * @file faio/detail/fs/platform/unix/file_state.hpp
 * @brief 文件共享状态。
 */
#pragma once
#include "faio/detail/fs/core/active_operations.hpp"
#include "faio/detail/fs/core/file_cursor.hpp"
#include "faio/detail/io/core/context_impl.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unistd.h>

namespace faio::fs::detail {
/** @brief 文件句柄、逻辑游标、活跃操作和停机清理的共享状态。 */
struct file_state {
  std::uint64_t
      shutdown_registration{}; /**< 关闭后移除弱回调，避免反复 open 时累积控制状态所占的内存。 */
  io::io_context context;      /**< 不依赖析构线程的 TLS，清理始终交给创建时的服务。 */
  std::atomic<int> descriptor{-1}; /**< 仅在所有操作租约排空后失效。 */
  bool append{};                   /**< 是否采用追加写入语义，区别于指定偏移写入。 */
  std::shared_ptr<cursor_state> cursor{
      std::make_shared<cursor_state>()}; /**< 共享文件位置或目录枚举游标，由对应的平台实现推进。 */
  std::mutex mutex;                      /**< 保护操作接纳、close 和活跃操作登记。 */
  bool closing{}, shutdown_pending{},
      shutdown_close_started{}; /**< 记录停机关闭操作是否已启动，防止重复关闭。 */
  std::optional<::faio::error>
      close_error; /**< 保存关闭过程中的错误，使重复关闭的等待者取得相同结果。 */
  ::faio::detail::completion_event
      close_completion; /**< 保留关闭完成状态的事件，所有关闭等待者共享该通知。 */

  active_file_operations active; /**< 记录尚未安全完成的文件操作，关闭和同步操作据此等待排空。 */

  /** @brief 判断文件状态是否持有有效原生句柄。 */
  bool valid() const noexcept { return descriptor.load() >= 0; }

  /** @brief 保存文件句柄、I/O 上下文和追加模式。 */
  file_state(io::io_context ctx, int fd, bool append_mode)
      : context(std::move(ctx)), descriptor(fd), append(append_mode) {}

  /** @brief 回收尚未关闭的文件句柄。 */
  ~file_state() {
    context.unregister_shutdown_cleanup(shutdown_registration);
    const int fd = descriptor.exchange(-1);
    if (fd >= 0 && !(context.domain() && context.domain()->defer_native_close(fd)))
      context.defer_cleanup([fd] { ::close(fd); });
  }
};

/**
 *  @brief 停机时在最后一个租约释放后提交关闭请求，无需辅助线程阻塞等待租约。
 * @details 通过 exchange 取出的唯一 fd 交由原生 CLOSE 或回退清理任务持有；完成事件必须
 *          等待实际的 CQE 或 close 结果，不能在移交句柄时提前通知。
 */
inline void finish_shutdown_file(const std::shared_ptr<file_state>& file) noexcept {
  int fd;
  {
    std::lock_guard lock(file->mutex);
    if (!file->shutdown_pending || !file->active.empty() || file->shutdown_close_started)
      return;
    file->shutdown_close_started = true;
    fd = file->descriptor.exchange(-1);
  }
  file->context.unregister_shutdown_cleanup(file->shutdown_registration);
  if (fd < 0) {
    file->close_completion.notify();
    file->context.domain()->release_cleanup_wait();
    return;
  }
  if (file->context.domain() && file->context.domain()->defer_native_close(fd, [file](int error) {
        if (error)
          file->close_error = make_error(error);
        file->close_completion.notify();
        file->context.domain()->release_cleanup_wait();
      }))
    return;
  file->context.defer_cleanup([file, fd] {
    if (::close(fd))
      file->close_error = make_error(errno);
    file->close_completion.notify();
    file->context.domain()->release_cleanup_wait();
  });
}

}  // namespace faio::fs::detail
