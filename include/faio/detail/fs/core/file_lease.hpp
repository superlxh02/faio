/**
 * @file faio/detail/fs/core/file_lease.hpp
 * @brief 文件与目录异步接口：文件操作租约。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include "faio/detail/common/error.hpp"
#include "faio/detail/fs/core/file_state.hpp"
#include <memory>
#include <mutex>
#include <utility>

namespace faio::fs::detail {
/** @brief 操作 admission 租约；析构发布 quiescent，供 sync/close 安全等待。 */
class file_lease {
 public:
  file_lease(std::shared_ptr<file_state> state, active_file_operations::handle operation) noexcept
      : state_(std::move(state)), operation_(std::move(operation)) {}

  file_lease(file_lease&&) = default;

  file_lease(const file_lease&) = delete;

  ~file_lease() {
    if (!operation_)
      return;
    bool shutdown_ready;
    {
      /// @brief 系统调用及游标更新已结束，清理线程才可关闭 fd。
      std::lock_guard lock(state_->mutex);
      state_->active.remove(operation_);
      shutdown_ready = state_->shutdown_pending && state_->active.empty();
    }
    /// @brief 最后一条IO实际完成后才允许停机CLOSE。
    if (shutdown_ready)
      finish_shutdown_file(state_);
    /// @brief close/flush 的协程观察同一排空点。
    operation_->completion.notify();
  }

  /// @brief 创建异步许可等待器，等待期间通过节点协议响应通知与取消。
  static expected<file_lease> acquire(const std::shared_ptr<file_state>& state,
                                      bool write = false) {
    if (!state)
      return std::unexpected{make_error(EBADF)};
    auto operation = std::make_shared<active_file_operations::operation>(write);
    std::lock_guard lock(state->mutex);
    if (state->closing || state->context.stopped() || !state->valid())
      return std::unexpected{make_error(EBADF)};
    state->active.insert(operation);
    return file_lease{state, std::move(operation)};
  }

 private:
  std::shared_ptr<file_state> state_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
  active_file_operations::handle operation_;  ///< 地址稳定的底层异步操作，当前包装负责其启动、等待与结果交付。
};
}
