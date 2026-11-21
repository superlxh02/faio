/**
 * @file faio/detail/process/exit_wait_operation.hpp
 * @brief 可取消的退出等待及完成发布责任。
 */
#pragma once
#include "faio/detail/process/child.hpp"
#include "faio/detail/process/process_service.hpp"
#include "faio/detail/coroutine/wait/completion_event.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/process/execution_adapter/operation_scope.hpp"
#endif
#include <stop_token>

namespace faio::process::detail {
/** @brief 在产生副作用前检查父取消、显式取消和已过期截止时间。 */
inline auto preflight(io::OperationOptions options, std::stop_token parent = {}) -> expected<void> {
  if (options.stop.stop_requested() || parent.stop_requested())
    return std::unexpected{make_error(ECANCELED)};
  if (options.deadline && std::chrono::steady_clock::now() >= *options.deadline)
    return std::unexpected{make_error(ETIMEDOUT)};
  return {};
}

/** @brief 将单次退出等待与运行时完成发布租约绑定的状态，地址保持不变。 */
class ExitWaitOperation : public std::enable_shared_from_this<ExitWaitOperation> {
 public:
  /** @brief 禁止复制退出等待状态，避免重复持有所有权或复制运行状态。 */
  ExitWaitOperation(const ExitWaitOperation&) = delete;
  /** @brief 退出等待状态不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const ExitWaitOperation&) -> ExitWaitOperation& = delete;
  /** @brief 退出等待状态不能移动；回调和协程依赖其固定地址。 */
  ExitWaitOperation(ExitWaitOperation&&) = delete;
  /** @brief 退出等待状态不支持移动赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(ExitWaitOperation&&) -> ExitWaitOperation& = delete;
  /** @brief 释放等待结果及租约；已接纳的等待应先完成排空。 */
  ~ExitWaitOperation() = default;

  /** @brief 分配地址固定的等待状态，避免回调在挂起后引用被移动的对象。 */
  static auto create(std::shared_ptr<ProcessState> state,
                     io::OperationOptions options,
                     std::stop_token parent) -> expected<std::shared_ptr<ExitWaitOperation>> {
    try {
      struct factory final : ExitWaitOperation {
        /** @brief 原样转发构造参数，通过 make_shared 在一次分配中创建地址固定的状态。 */
        factory(std::shared_ptr<ProcessState> owned,
                io::OperationOptions operation,
                std::stop_token token)
            : ExitWaitOperation(std::move(owned), operation, token) {}
      };

      return std::make_shared<factory>(std::move(state), options, parent);
    } catch (const std::bad_alloc&) {
      return std::unexpected{make_error(ENOMEM)};
    }
  }

  /** @brief 登记唯一等待者和两个取消回调，在进程退出或首次等待失败后撤下等待者。 */
  auto wait() -> task<expected<ExitStatus>>;

  /** @brief 从进程记录读取已缓存的终态，并尝试完成本次等待。 */
  void notify_terminal() noexcept {
    auto result = state_->cached_exit();
    if (!result)
      finish(std::unexpected{result.error()});
    else if (*result)
      finish(**result);
  }

  /** @brief 保存本次请求的取消原因，并推动尚未交付的工作结束。 */
  void request_stop(Error error) noexcept { finish(std::unexpected{std::move(error)}); }

  /** @brief 从进程记录中撤下本等待者，腾出位置供后续等待登记。 */
  void unlink() noexcept { state_->detach_waiter(shared_from_this()); }

  /** @brief 返回本次退出等待使用的绝对截止时间。 */
  auto deadline() const noexcept { return options_.deadline; }

 private:
  /** @brief 固定本次等待的进程记录、操作选项及父取消令牌。 */
  ExitWaitOperation(std::shared_ptr<ProcessState> state,
                    io::OperationOptions options,
                    std::stop_token parent)
      : state_(std::move(state)), options_(options), parent_(parent) {}

  /** @brief 在结果锁下只发布一次等待结果，随后持有发布租约唤醒协程。 */
  void finish(expected<ExitStatus> value) noexcept {
    {
      std::lock_guard lock(mutex_);
      if (result_)
        return;
      result_.emplace(std::move(value));
    }
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    auto publisher = execution_scope_.start_publisher();
#endif
    completion_.notify();
  }

  std::shared_ptr<ProcessState> state_; /**< 等待结束前保持原生进程记录有效。 */
  io::OperationOptions options_;        /**< 本次退出等待的显式取消及绝对截止时间。 */
  std::stop_token parent_;              /**< 创建操作时继承的父取消令牌。 */
  std::mutex mutex_;                    /**< 保护单次等待的唯一完成结果。 */
  std::optional<expected<ExitStatus>>
      result_; /**< 进程退出、取消、到期或运行时关闭中最先产生的结果。 */
  ::faio::detail::completion_event
      completion_; /**< 不可提前放弃的完成通知，保证结果发布安全排空。 */
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  execution_adapter::operation_scope execution_scope_;
#endif
};

/** @brief 登记唯一等待者和两个取消回调，在进程退出或首次等待失败后撤下等待者。 */
inline auto ExitWaitOperation::wait() -> task<expected<ExitStatus>> {
  auto keep = shared_from_this();
  if (auto checked = preflight(options_, parent_); !checked)
    co_return std::unexpected{checked.error()};
  if (state_->runtime_stopped())
    co_return std::unexpected{process_error(errc::runtime_shutdown)};
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  execution_scope_.capture();
  auto lease = execution_scope_.acquire_child();
#endif
  auto attached = state_->attach_waiter(keep);
  if (!attached)
    co_return std::unexpected{attached.error()};
  if (!*attached) {
    auto cached = state_->cached_exit();
    if (!cached)
      co_return std::unexpected{cached.error()};
    co_return **cached;
  }
  auto stop = [weak = std::weak_ptr{keep}] {
    if (auto self = weak.lock())
      self->request_stop(make_error(ECANCELED));
  };
  std::stop_callback parent_callback{parent_, stop};
  std::stop_callback explicit_callback{options_.stop, stop};
  if (auto service = ProcessService::instance())
    (*service)->notify_control_change();
  co_await completion_.wait();
  unlink();
  std::lock_guard lock(mutex_);
  co_return std::move(*result_);
}
}  // namespace faio::process::detail
