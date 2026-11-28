/**
 * @file faio/detail/fs/platform/windows/file_state.hpp
 * @brief 文件共享状态。
 */
#pragma once
#include "faio/detail/fs/core/active_operations.hpp"
#include "faio/detail/fs/core/file_cursor.hpp"
#include "faio/detail/fs/platform/windows/native.hpp"
#include "faio/detail/io/engine.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace faio::fs::detail {
/**
 *  @brief 所有 File 关闭路径都先移除后端注册缓存，避免 HANDLE 数值复用时误用旧注册。
 * @details 只能在全部操作租约排空后调用。操作系统中的 IOCP 关联随句柄释放，
 *          域缓存须先移除旧代际的记录，下次打开时才能重新建立正确关联。
 */
inline expected<void> close_file_handle(const io::io_context& context, HANDLE handle) noexcept {
  if (!valid_handle(handle))
    return {};
  if (context.domain())
    context.domain()->forget_native_handle(reinterpret_cast<std::intptr_t>(handle),
                                           io::detail::native_handle_kind::WINDOWS_HANDLE);
  if (!::CloseHandle(handle))
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  return {};
}

/**  @brief 持有文件状态，使 HANDLE、借用关系和关闭状态均不依赖协程对象的地址。 */
struct file_state {
  std::uint64_t shutdown_registration{}; /**< 弱清理回调，在关闭后注销。 */
  io::io_context context;                /**< 保留创建时的 IO 域，不依赖恢复线程的 TLS。 */
  std::atomic<HANDLE> descriptor{INVALID_HANDLE_VALUE}; /**< 保留完整指针宽度。 */
  bool append{};  /**< append 写由内核使用全一偏移保证原子追加。 */
  DWORD access{}; /**< 保存创建权限，ReOpenFile 克隆不扩大原句柄权限。 */
  std::shared_ptr<cursor_state> cursor{
      std::make_shared<cursor_state>()}; /**< 共享文件位置或目录枚举游标，由对应的平台实现推进。 */
  std::mutex mutex; /**< 只保护操作接纳、租约和关闭状态，不在内核调用期间持锁。 */
  bool closing{}, shutdown_pending{},
      shutdown_close_started{}; /**< 记录停机关闭操作是否已启动，防止重复关闭。 */
  std::optional<::faio::error>
      close_error; /**< 保存关闭过程中的错误，使重复关闭的等待者取得相同结果。 */
  ::faio::detail::completion_event
      close_completion; /**< 保留关闭完成状态的事件，所有关闭等待者共享该通知。 */

  active_file_operations active; /**< 记录尚未安全完成的文件操作，关闭和同步操作据此等待排空。 */

  /** @brief 判断文件状态是否持有有效原生句柄。 */
  bool valid() const noexcept { return valid_handle(descriptor.load()); }

  /** @brief 保存文件句柄、I/O 上下文和追加模式。 */
  file_state(io::io_context owner, HANDLE handle, bool append_mode, DWORD access_mask)
      : context(std::move(owner)), descriptor(handle), append(append_mode), access(access_mask) {}

  /** @brief 回收尚未关闭的文件句柄。 */
  ~file_state() {
    context.unregister_shutdown_cleanup(shutdown_registration);
    const HANDLE handle = descriptor.exchange(INVALID_HANDLE_VALUE);
    if (valid_handle(handle))
      context.defer_cleanup([owner = context, handle] { (void)close_file_handle(owner, handle); });
  }
};

/**  @brief 停机时，等待最后一个已接纳的租约释放后再关闭 HANDLE。 */
inline void finish_shutdown_file(const std::shared_ptr<file_state>& state) noexcept {
  HANDLE handle;
  {
    std::lock_guard lock(state->mutex);
    if (!state->shutdown_pending || !state->active.empty() || state->shutdown_close_started)
      return;
    state->shutdown_close_started = true;
    handle = state->descriptor.exchange(INVALID_HANDLE_VALUE);
  }
  state->context.unregister_shutdown_cleanup(state->shutdown_registration);
  state->context.defer_cleanup([state, handle] {
    auto result = close_file_handle(state->context, handle);
    if (!result)
      state->close_error = result.error();
    state->close_completion.notify();
    state->context.domain()->release_cleanup_wait();
  });
}

}  // namespace faio::fs::detail
