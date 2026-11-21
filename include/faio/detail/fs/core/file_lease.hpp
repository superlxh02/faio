/**
 * @file faio/detail/fs/core/file_lease.hpp
 * @brief 文件操作租约。
 */
#pragma once
#include "faio/detail/common/error.hpp"
#include "faio/detail/fs/core/file_state.hpp"
#include <memory>
#include <mutex>
#include <utility>

namespace faio::fs::detail {
/**  @brief 文件操作的接纳租约，析构时发布操作已结束的状态，供 sync/close 安全等待。 */
class file_lease {
 public:
  /** @brief 持有文件状态及已登记操作，析构时发布完成。 */
  file_lease(std::shared_ptr<file_state> state, active_file_operations::handle operation) noexcept
      : state_(std::move(state)), operation_(std::move(operation)) {}

  /** @brief 转移文件状态和操作租约。 */
  file_lease(file_lease&&) = default;

  /** @brief 禁止复制此对象。 */
  file_lease(const file_lease&) = delete;

  /** @brief 撤销活跃文件操作登记并发布完成事件。 */
  ~file_lease() {
    if (!operation_)
      return;
    bool shutdown_ready;
    {
      std::lock_guard lock(state_->mutex);
      state_->active.remove(operation_);
      shutdown_ready = state_->shutdown_pending && state_->active.empty();
    }
    if (shutdown_ready)
      finish_shutdown_file(state_);
    operation_->completion.notify();
  }

  /** @brief 同步登记文件操作并取得租约，已关闭文件返回错误。 */
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
  std::shared_ptr<file_state>
      state_; /**< 共享持有操作状态，保证其在异步执行和领取结果期间地址不变。 */
  active_file_operations::handle
      operation_; /**< 活跃文件操作的共享记录，在租约析构时撤销登记并发布完成。 */
};
}  // namespace faio::fs::detail
