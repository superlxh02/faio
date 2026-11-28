/**
 * @file faio/detail/process/process_state.hpp
 * @brief 与运行时弱关联的进程记录和唯一退出等待门。
 */
#pragma once
#include "faio/detail/process/types.hpp"
#include "faio/detail/io/core/context.hpp"
#include <atomic>
#include <memory>
#include <mutex>

namespace faio::process {
/** @brief 已启动进程及标准管道的独占控制器。 */
class child;
}  // namespace faio::process

namespace faio::process::detail {
/** @brief 整个程序共享的有界启动与回收服务，无需为每个子进程单独创建线程。 */
class process_service;
/** @brief 将单次退出等待与运行时完成发布租约绑定的状态，地址保持不变。 */
class exit_wait_operation;
/** @brief 持有尚未交给监视服务的原生进程责任，析构时清理启动失败留下的资源。 */
class native_process;

/** @brief 以唯一代际令牌标识的进程记录，串行缓存退出结果。 */
class process_state : public std::enable_shared_from_this<process_state> {
 public:
  /** @brief 禁止复制进程记录，避免重复持有所有权或复制运行状态。 */
  process_state(const process_state&) = delete;
  /** @brief 进程记录不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const process_state&) -> process_state& = delete;
  /** @brief 进程记录不能移动；回调和协程依赖其固定地址。 */
  process_state(process_state&&) = delete;
  /** @brief 进程记录不支持移动赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(process_state&&) -> process_state& = delete;
  /** @brief 撤销运行时关闭回调并释放仍由记录持有的平台监视资源。 */
  ~process_state();

  /** @brief 返回尚未确认退出的进程 ID；无控制权或终态时为空。 */
  auto id() const noexcept -> std::optional<process_id> {
    std::lock_guard lock(mutex_);
    return terminal_ || !pid_ ? std::nullopt : std::optional{pid_};
  }

  /** @brief 返回不可复用的记录代际令牌，避免旧监视通知误命中新记录。 */
  auto record_token() const noexcept -> std::uint64_t { return token_; }

  /** @brief 复制已经缓存的退出结果，尚未退出时返回空 optional。 */
  auto cached_exit() const -> expected<std::optional<exit_status>> {
    std::lock_guard lock(mutex_);
    if (error_)
      return std::unexpected{*error_};
    return status_;
  }

  /** @brief 独占 wait/kill 的退出等待入口，已有等待者时立即报忙。 */
  auto try_acquire_exit_gate() -> expected<void> {
    std::lock_guard lock(mutex_);
    if (gate_)
      return std::unexpected{process_error(errc::OPERATION_IN_PROGRESS)};
    gate_ = true;
    return {};
  }

  /** @brief 归还退出等待入口，使取消后的调用可以重试。 */
  void release_exit_gate() noexcept {
    std::lock_guard lock(mutex_);
    gate_ = false;
  }

  /** @brief 在退出记录上安装一个弱等待者；已终止时无需挂起。 */
  auto attach_waiter(const std::shared_ptr<exit_wait_operation>&) -> expected<bool>;
  /** @brief 仅移除仍与本次操作相同的等待者，避免误删后续等待。 */
  void detach_waiter(const std::shared_ptr<exit_wait_operation>&) noexcept;
  /** @brief 在结果锁内发布首个终态，再在锁外唤醒等待者。 */
  auto commit_exit(expected<exit_status>) -> bool;

  /** @brief 检查原 IO 域是否已关闭或已经释放。 */
  auto runtime_stopped() const noexcept -> bool {
    auto domain = runtime_.lock();
    return runtime_stopped_.load() || !domain || io::io_context{domain}.stopped();
  }

  /** @brief 停止本运行时上的等待，不阻止独立服务继续回收子进程。 */
  void release_runtime_attachment() noexcept;
  /** @brief 通过弱引用登记运行时关闭回调，空闲子进程不会延长运行时的生命周期。 */
  void attach_runtime(const io::io_context&);

 private:
  /** @brief 允许共享服务推进启动状态、登记监视资源并归还记录槽。 */
  friend class process_service;
  /** @brief 允许启动准备对象在回滚时检查原生创建是否已经完成。 */
  friend class prepared_launch;
  /** @brief 允许未交接的原生进程把失败清理转回服务。 */
  friend class native_process;
  /** @brief 允许退出等待在记录上安装和撤下唯一等待者。 */
  friend class exit_wait_operation;
  /** @brief 允许终止请求在记录锁内查询和操作该记录对应的子进程。 */
  friend auto request_termination(const std::shared_ptr<process_state>&) -> expected<void>;
  /** @brief 允许退出查询在同一记录锁下缓存终态。 */
  friend auto poll_exit(const std::shared_ptr<process_state>&)
      -> expected<std::optional<exit_status>>;
  /** @brief 允许平台启动实现在不可分割的操作中填写预留记录的原生标识。 */
  friend auto launch_native(/** 持有待启动进程的参数、管道和预留的监视资源。 */
                            class prepared_launch&) -> expected<native_process>;
#if defined(_WIN32)
  /** @brief 允许公共控制器借用 Windows 原生进程句柄。 */
  friend class ::faio::process::child;
#endif
  /** @brief 固定记录代际令牌，尚未绑定原生进程。 */
  explicit process_state(std::uint64_t token) : token_(token) {}

  /** @brief 持有记录锁时，以非阻塞方式查询该记录对应的进程，并缓存首个终态。 */
  auto poll_locked() -> expected<std::optional<exit_status>>;
  /** @brief 从等待者的弱引用取得强引用后，在记录锁外发布完成。 */
  void notify_waiter();
  mutable std::mutex mutex_;                  /**< 串行保护原生标识、退出缓存、等待者和监视字段。 */
  process_id pid_{};                          /**< 创建成功后固定的进程 ID，零表示尚未创建。 */
  const std::uint64_t token_;                 /**< 服务分配的唯一代际令牌，用于区分重用记录槽。 */
  bool launching_{};                          /**< 记录已预留但原生创建尚未完成。 */
  bool watch_ready_{};                        /**< 监视资源已登记或明确进入回退回收路径。 */
  bool terminal_{};                           /**< 退出状态或不可恢复的查询错误已经缓存。 */
  bool gate_{};                               /**< 公共 wait 或 kill 已占用唯一退出等待门。 */
  bool public_control_released_{};            /**< 公共 Child 已交还控制权，服务仍可独立回收。 */
  bool kill_requested_{};                     /**< 已向该记录对应的进程发出强制终止请求。 */
  std::optional<exit_status> status_;         /**< 可由后续 wait 重复观察的退出状态。 */
  std::optional<error> error_;                /**< 原生退出查询的持久失败，避免重复消费原生终态。 */
  std::weak_ptr<exit_wait_operation> waiter_; /**< 当前异步等待者的弱引用，避免记录与等待帧成环。 */
  std::weak_ptr<io::detail::io_domain>
      runtime_; /**< 原 I/O 域的弱引用，空闲 Child 不会延长运行时的生命周期。 */
  std::atomic<bool> runtime_stopped_{}; /**< 运行时关闭回调发布的停止标记。 */
  std::uint64_t shutdown_token_{};      /**< 用于在记录析构时撤销运行时关闭回调。 */
#if defined(_WIN32)
  HANDLE process_{};
  PTP_WAIT native_wait_{};
#else
  int watch_fd_{-1};
  bool fallback_{};
#endif
};
}  // namespace faio::process::detail
