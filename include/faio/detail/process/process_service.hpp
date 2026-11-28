/**
 * @file faio/detail/process/process_service.hpp
 * @brief 有界启动队列与平台进程退出监视服务。
 */
#pragma once
#include "faio/detail/process/process_state.hpp"
#include <array>
#include <condition_variable>
#include <deque>
#include <thread>

namespace faio::process::detail {
/** @brief 在调用协程与启动线程之间传递请求和唯一结果的状态，地址保持不变。 */
class launch_operation;

/** @brief 整个程序共享的有界启动与回收服务，无需为每个子进程单独创建线程。 */
class process_service {
 public:
  /** @brief 禁止复制共享启动与回收服务，避免重复持有所有权或复制运行状态。 */
  process_service(const process_service&) = delete;
  /** @brief 共享启动与回收服务不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const process_service&) -> process_service& = delete;
  /** @brief 共享启动与回收服务不能移动；回调和协程依赖其固定地址。 */
  process_service(process_service&&) = delete;
  /** @brief 共享启动与回收服务不支持移动赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(process_service&&) -> process_service& = delete;
  /** @brief 按需取得程序共享服务，资源创建失败时转换为 expected 错误。 */
  static auto instance() -> expected<process_service*>;
  /** @brief 从固定容量记录表预留空槽，并分配新的代际令牌。 */
  auto reserve_record() -> expected<std::shared_ptr<process_state>>;
  /** @brief 将启动请求加入有界队列；队列已满时立即拒绝。 */
  auto enqueue_launch(std::shared_ptr<launch_operation>) -> expected<void>;
  /** @brief 从启动队列移除尚未执行的请求，再发布其取消结果。 */
  void cancel_queued(const std::shared_ptr<launch_operation>&) noexcept;
  /** @brief 注册平台退出通知并接管原生进程的后续回收责任。 */
  auto watch(const std::shared_ptr<process_state>&, native_process) -> expected<void>;
  /** @brief 增加控制版本并唤醒服务线程重新计算工作与等待期限。 */
  void notify_control_change() noexcept;
  /** @brief 记录公共控制器已释放，并按策略请求终止子进程。 */
  void release_public_control(const std::shared_ptr<process_state>&, bool) noexcept;
  /** @brief 取消指定 IO 域上的活动等待，保留原生进程回收责任。 */
  void detach_runtime(const io::io_context&) noexcept;

  /** @brief 统计仍占用服务记录表的进程，供资源回归诊断。 */
  auto active_records() const noexcept -> std::size_t {
    std::lock_guard lock(mutex_);
    std::size_t count{};
    for (const auto& s : records_)
      count += !!s;
    return count;
  }

  /** @brief 停止固定服务线程并等待其结束，再排空平台退出通知的清理工作。 */
  ~process_service();

 private:
  /** @brief 允许未交接的原生进程把失败清理转回服务。 */
  friend class native_process;
  /** @brief 允许启动准备对象在回滚时检查原生创建是否已经完成。 */
  friend class prepared_launch;
  /** @brief 建立平台控制通知源并启动固定数量的服务线程。 */
  process_service();
  /** @brief 由固定启动线程串行执行原生创建并协调 Windows 退出清理。 */
  void launch_loop() noexcept;
#if !defined(_WIN32)
  /** @brief 由 Unix 固定监视线程等待退出通知、超时和控制变更。 */
  void monitor_loop() noexcept;
#endif
  /** @brief 查询已注册记录，结束已到期的等待，并在终态后释放平台监视资源。 */
  void sweep() noexcept;
  /** @brief 根据等待截止时间及必要的轮询回退，选择最近的唤醒时间。 */
  auto next_deadline() -> std::optional<std::chrono::steady_clock::time_point>;
  /** @brief 移除指定记录所占槽位，释放预留失败或已回收的服务容量。 */
  void abandon_reserved(const std::shared_ptr<process_state>&) noexcept;
  mutable std::mutex mutex_;   /**< 串行保护记录表、启动队列及服务接纳。 */
  std::condition_variable cv_; /**< 唤醒固定启动线程处理队列和 Windows 控制变更。 */
  std::array<std::shared_ptr<process_state>, 4096>
      records_{}; /**< 固定容量的活进程和启动预留记录表。 */
  std::deque<std::shared_ptr<launch_operation>>
      launches_;                          /**< 最多接纳二百五十六项的原生启动请求队列。 */
  std::atomic<bool> stopping_{};          /**< 停止接纳新请求并推动服务线程退出。 */
  std::uint64_t next_token_{};            /**< 在服务锁内递增的记录代际令牌来源。 */
  std::atomic<std::uint64_t> revision_{}; /**< 控制变更版本，防止条件变量通知先于等待而丢失。 */
  std::thread launch_;                    /**< 唯一执行可能阻塞的原生创建操作的服务线程。 */
#if defined(_WIN32)
  /** @brief 在 Windows 等待回调中缓存退出并唤醒服务，回调自身不做排空等待。 */
  static void CALLBACK native_callback(PTP_CALLBACK_INSTANCE,
                                       void*,
                                       PTP_WAIT,
                                       TP_WAIT_RESULT) noexcept;
#else
  std::thread monitor_;
  int poller_{-1};
#if defined(__linux__)
  int control_{-1};
#endif
#endif
};
}  // namespace faio::process::detail
