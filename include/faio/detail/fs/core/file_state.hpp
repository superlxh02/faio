/**
 * @file faio/detail/fs/core/file_state.hpp
 * @brief 文件共享状态。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/platform/windows/file_state.hpp"
#else
#include "faio/detail/fs/platform/unix/file_state.hpp"
#endif

namespace faio::fs::detail {
/**  @brief 通过弱引用登记停机回调，避免 File 与 domain 形成引用环。 */
inline void register_file_shutdown(const std::shared_ptr<file_state>& state) {
  std::weak_ptr<file_state> weak = state;
  state->shutdown_registration = state->context.register_shutdown_cleanup([weak] {
    if (auto file = weak.lock()) {
      {
        std::lock_guard lock(file->mutex);
        if (file->closing)
          return;
        file->closing = true;
        file->shutdown_pending = true;
        file->context.domain()->retain_cleanup_wait();
      }
      finish_shutdown_file(file);
    }
  });
}
}  // namespace faio::fs::detail
