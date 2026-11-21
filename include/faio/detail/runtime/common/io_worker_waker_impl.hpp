/**
 * @file faio/detail/runtime/common/io_worker_waker_impl.hpp
 * @brief IO 域完整后定义具体唤醒桥，不引入 timer 或运行时组合层。
 */
#pragma once
#include "faio/detail/runtime/common/worker_waker_ref.hpp"
#include "faio/detail/io/core/domain.hpp"

namespace faio::runtime::detail {
/** @brief 唤醒工作线程处理已发布工作。 */
inline void io_worker_waker::wake_up() const noexcept {
  domain_->wake();
}
}  // namespace faio::runtime::detail
