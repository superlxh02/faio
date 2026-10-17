/**
 * @file faio/detail/fs/core/file_state.hpp
 * @brief 文件与目录异步接口：文件共享状态。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
/** @brief 原生文件拥有状态选择；租约和逻辑游标协议由公共 core 提供。 */
#if defined(_WIN32)
#include "faio/detail/fs/platform/windows/file_state.hpp"
#else
#include "faio/detail/fs/platform/unix/file_state.hpp"
#endif

namespace faio::fs::detail {
/** @brief 注册弱停机回调，不形成 File/domain 引用环。 */
inline void register_file_shutdown(const std::shared_ptr<file_state>& state) {
  std::weak_ptr<file_state> weak = state;
  state->shutdown_registration = state->context.register_shutdown_cleanup([weak] {
    if (auto file = weak.lock()) {
      {
        std::lock_guard lock(file->mutex);
        /// @brief 已有显式close负责排空和提交，不争抢其关闭责任。
        if (file->closing)
          return;
        file->closing = true;
        file->shutdown_pending = true;
        /// @brief 等最后lease期间仍计入drain。
        file->context.domain()->retain_cleanup_wait();
      }
      finish_shutdown_file(file);
    }
  });
}
}
