/**
 * @file faio/detail/fs/platform/unix/file_state.hpp
 * @brief 文件与目录异步接口：文件共享状态。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include "faio/detail/fs/core/active_operations.hpp"
#include "faio/detail/fs/core/file_cursor.hpp"
#include "faio/detail/io/engine.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unistd.h>

namespace faio::fs::detail {
struct file_state {
  std::uint64_t shutdown_registration{};  ///< 关闭后删除弱回调，重复open不累计控制面内存。
  io::io_context context;  ///< 不依赖析构线程的 TLS，清理始终交给创建时的服务。
  std::atomic<int> descriptor{-1};  ///< 仅在所有操作租约排空后失效。
  bool append{};  ///< 是否采用追加写入语义，区别于指定偏移写入。
  std::shared_ptr<cursor_state> cursor{std::make_shared<cursor_state>()};  ///< 共享文件位置或目录枚举游标，按具体平台实现推进。
  std::mutex mutex;  ///< 保护 admission、close 与活跃操作登记。
  bool closing{},  ///< 资源已经进入关闭阶段，拒绝新的业务操作。
      shutdown_pending{},  ///< 域停机是否已登记本资源的关闭请求。
      shutdown_close_started{};  ///< 停机关闭操作是否已真正启动，防止重复关闭。
  std::optional<Error> close_error;  ///< 关闭过程保存的错误，供重复关闭等待者取得同一结果。
  ::faio::detail::completion_event close_completion;  ///< 带记忆的关闭完成事件，所有关闭等待者共享同一完成通知。

  active_file_operations active;  ///< 记录尚未安全完成的文件操作，关闭与同步查询据此等待排空。

  /// @brief 检查当前包装是否持有有效资源身份。
  bool valid() const noexcept { return descriptor.load() >= 0; }

  file_state(io::io_context ctx, int fd, bool append_mode)
      : context(std::move(ctx)), descriptor(fd), append(append_mode) {}

  ~file_state() {
    context.unregister_shutdown_cleanup(shutdown_registration);
    /// @brief 最后一个操作 lease 已销毁才会进入这里；清理不在 worker 同步等待。
    const int fd = descriptor.exchange(-1);
    if (fd >= 0 && !(context.domain() && context.domain()->defer_native_close(fd)))
      context.defer_cleanup([fd] { ::close(fd); });
  }
};

/** @brief 停机关闭在最后一个租约释放后提交，不用辅助线程阻塞等待租约。
 * @details fd唯一exchange之后属于原生CLOSE或fallback清理任务；完成事件必须
 *          等真实CQE/close结果，不能在交接句柄时提前通知。
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
  /// @brief 原生控制记录拥有file租约至最终CQE，不创建清理线程job。
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

}
