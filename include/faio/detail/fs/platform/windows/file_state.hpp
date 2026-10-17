/**
 * @file faio/detail/fs/platform/windows/file_state.hpp
 * @brief 文件与目录异步接口：文件共享状态。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
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
/** @brief 所有 File 关闭路径先移除后端注册缓存，防止同数值 HANDLE 被复用。
 * @details 只能在全部操作 lease 排空后调用。IOCP 的 OS 关联随真实句柄释放，
 *          域缓存必须先忘记旧代际，下一次打开才会重新执行正确的关联。
 */
inline expected<void> close_file_handle(const io::io_context& context, HANDLE handle) noexcept {
  if (!valid_handle(handle))
    return {};
  if (context.domain())
    context.domain()->forget_native_handle(reinterpret_cast<std::intptr_t>(handle),
                                           io::detail::native_handle_kind::windows_handle);
  if (!::CloseHandle(handle))
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  return {};
}

/** @brief 文件拥有状态；HANDLE、借用和关闭控制面均与协程对象地址解耦。 */
struct file_state {
  std::uint64_t shutdown_registration{};  ///< 弱清理回调，在关闭后注销。
  io::io_context context;  ///< 固定创建 IO 域，不依赖恢复线程 TLS。
  std::atomic<HANDLE> descriptor{INVALID_HANDLE_VALUE};  ///< 保留完整指针宽度。
  bool append{};  ///< append 写由内核使用全一偏移保证原子追加。
  DWORD access{};  ///< 保存创建权限，ReOpenFile 克隆不扩大原句柄权限。
  std::shared_ptr<cursor_state> cursor{std::make_shared<cursor_state>()};  ///< 共享文件位置或目录枚举游标，按具体平台实现推进。
  std::mutex mutex;  ///< 只保护 admission/lease/close，不跨内核调用持有。
  bool closing{},  ///< 资源已经进入关闭阶段，拒绝新的业务操作。
      shutdown_pending{},  ///< 域停机是否已登记本资源的关闭请求。
      shutdown_close_started{};  ///< 停机关闭操作是否已真正启动，防止重复关闭。
  std::optional<Error> close_error;  ///< 关闭过程保存的错误，供重复关闭等待者取得同一结果。
  ::faio::detail::completion_event close_completion;  ///< 带记忆的关闭完成事件，所有关闭等待者共享同一完成通知。

  active_file_operations active;  ///< 记录尚未安全完成的文件操作，关闭与同步查询据此等待排空。

  /// @brief 检查当前包装是否持有有效资源身份。
  bool valid() const noexcept { return valid_handle(descriptor.load()); }

  file_state(io::io_context owner, HANDLE handle, bool append_mode, DWORD access_mask)
      : context(std::move(owner)), descriptor(handle), append(append_mode), access(access_mask) {}

  ~file_state() {
    context.unregister_shutdown_cleanup(shutdown_registration);
    /// @brief 唯一移交点。
    const HANDLE handle = descriptor.exchange(INVALID_HANDLE_VALUE);
    /// @brief worker 不等待关闭。
    if (valid_handle(handle))
      context.defer_cleanup([owner = context, handle] {
        (void)close_file_handle(owner, handle);
      });
  }
};

/** @brief 停机只在最后一个已接受 lease 释放后关闭真实 HANDLE。 */
inline void finish_shutdown_file(const std::shared_ptr<file_state>& state) noexcept {
  HANDLE handle;
  {
    std::lock_guard lock(state->mutex);
    if (!state->shutdown_pending || !state->active.empty() || state->shutdown_close_started)
      return;
    /// @brief 一个关闭责任，防止析构与停机重复关闭。
    state->shutdown_close_started = true;
    handle = state->descriptor.exchange(INVALID_HANDLE_VALUE);
  }
  state->context.unregister_shutdown_cleanup(state->shutdown_registration);
  state->context.defer_cleanup([state, handle] {
    auto result = close_file_handle(state->context, handle);
    if (!result)
      state->close_error = result.error();
    /// @brief 真实 CloseHandle 返回后才发布完成。
    state->close_completion.notify();
    /// @brief 覆盖等待 lease 的整个停机阶段。
    state->context.domain()->release_cleanup_wait();
  });
}

}
