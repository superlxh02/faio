/**
 * @file faio/detail/io/core/context_impl.hpp
 * @brief 跨平台异步 IO：上下文转发实现。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/io/core/context.hpp"
#include "faio/detail/io/core/domain.hpp"

namespace faio::io {
/// @brief 取得当前线程已安装的上下文；缺少绑定时按接口约定返回空值。
inline io_context io_context::current() {
  return detail::current_domain ? io_context{detail::current_domain->shared_from_this()}
                                : io_context{};
}

/// @brief 读取当前组件是否已进入停止状态。
inline bool io_context::stopped() const noexcept {
  return !domain_ || domain_->stopped();
}

/// @brief 取得所属任务或服务的协作停止令牌。
inline std::stop_token io_context::stop_token() const noexcept {
  return domain_ ? domain_->stop_token() : std::stop_token{};
}

/// @brief 读取后端累计提交、完成与控制事件统计。
inline detail::backend_statistics io_context::statistics() const noexcept {
  return domain_ ? domain_->statistics() : detail::backend_statistics{};
}

/// @brief 取得文件或通用阻塞执行服务引用，具体服务归属由上下文决定。
inline execution::blocking_executor_ref io_context::blocking() const noexcept {
  return domain_ ? domain_->blocking() : execution::blocking_executor_ref{};
}

/// @brief 取得专门执行不可丢弃关闭工作的清理服务引用。
inline execution::blocking_executor_ref io_context::cleanup() const noexcept {
  return domain_ ? domain_->cleanup() : execution::blocking_executor_ref{};
}

/// @brief 取得隔离地址解析阻塞调用的执行服务引用。
inline execution::blocking_executor_ref io_context::resolver() const noexcept {
  return domain_ ? domain_->resolver() : execution::blocking_executor_ref{};
}

/// @brief 从运行时放置服务选择 IO 域，未配置放置服务时沿用当前域。
inline io_context io_context::balanced_context() const noexcept {
  return domain_ ? domain_->balanced_context() : io_context{};
}

/// @brief 持久保存必须执行的清理工作，服务饱和时由后续驱动重试。
inline void io_context::defer_cleanup(::faio::move_only_function<void()> cleanup) const noexcept {
  if (domain_)
    domain_->defer_cleanup(std::move(cleanup));
  else
    cleanup();
}

/// @brief 登记拥有型停机清理回调，使服务关闭时仍能归还长期资源。
inline std::uint64_t io_context::register_shutdown_cleanup(
    ::faio::move_only_function<void()> cleanup) const {
  return domain_ ? domain_->register_shutdown_cleanup(std::move(cleanup)) : 0;
}

/// @brief 撤销已经由正常关闭路径接管的停机清理责任。
inline void io_context::unregister_shutdown_cleanup(std::uint64_t token) const noexcept {
  if (domain_)
    domain_->unregister_shutdown_cleanup(token);
}

}
