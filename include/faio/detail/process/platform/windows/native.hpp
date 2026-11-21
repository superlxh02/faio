/**
 * @file faio/detail/process/platform/windows/native.hpp
 * @brief Windows 原生进程和待启动资源的独占所有权。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/process/command_snapshot.hpp"
#include "faio/detail/process/child.hpp"
#include "faio/detail/process/process_service.hpp"
#include "faio/detail/process/platform/windows/pipe.hpp"

namespace faio::process::detail {
/** @brief 持有尚未交给监视服务的原生进程责任，析构时清理启动失败留下的资源。 */
class NativeProcess {
 public:
  /** @brief 禁止复制尚未交接的进程清理责任，避免重复持有所有权或复制运行状态。 */
  NativeProcess(const NativeProcess&) = delete;
  /** @brief 尚未交接的进程清理责任不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const NativeProcess&) -> NativeProcess& = delete;

  /** @brief 转交尚未交接的进程记录和清理责任，将移动源 ID 归零。 */
  NativeProcess(NativeProcess&& other) noexcept
      : record_(std::move(other.record_)), pid_(std::exchange(other.pid_, 0)) {}

  /** @brief 清理原有尚未交接的进程，再接管新记录及进程 ID。 */
  auto operator=(NativeProcess&& other) noexcept -> NativeProcess& {
    if (this != &other) {
      cleanup();
      record_ = std::move(other.record_);
      pid_ = std::exchange(other.pid_, 0);
    }
    return *this;
  }

  /** @brief 终止尚未由服务接管的原生进程，并保留服务的回收路径。 */
  ~NativeProcess() { cleanup(); }

  /** @brief 返回尚未交接的原生进程 ID，清理或服务交接后为零。 */
  auto id() const noexcept -> process_id { return pid_; }

  /** @brief 借用记录中的 Windows 进程句柄。 */
  auto handle() const noexcept -> void* { return record_ ? record_->process_ : nullptr; }

 private:
  /** @brief 允许共享服务推进启动状态、登记监视资源并归还记录槽。 */
  friend class ProcessService;
  /** @brief 允许平台启动实现在不可分割的操作中填写预留记录的原生标识。 */
  friend auto launch_native(/** @brief 持有待启动进程的参数、管道和预留的监视资源。 */
                            class PreparedLaunch&) -> expected<NativeProcess>;

  /** @brief 接管尚未完成服务登记的原生进程清理责任。 */
  NativeProcess(std::shared_ptr<ProcessState> record, process_id pid)
      : record_(std::move(record)), pid_(pid) {}

  /** @brief 对未完成服务交接的进程请求终止，并恢复服务的回收路径。 */
  void cleanup() noexcept;
  std::shared_ptr<ProcessState>
      record_;       /**< 预留或已创建进程的记录，地址保持不变，并持有进程句柄。 */
  process_id pid_{}; /**< 尚未完成服务交接的 Windows 进程 ID。 */
};

/** @brief 预留进程记录并拥有全部启动前资源，失败时自动归还。 */
class PreparedLaunch {
 public:
  /** @brief 禁止复制待启动快照、记录及标准流资源，避免重复持有所有权或复制运行状态。 */
  PreparedLaunch(const PreparedLaunch&) = delete;
  /** @brief 待启动快照、记录及标准流资源不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const PreparedLaunch&) -> PreparedLaunch& = delete;
  /** @brief 转交启动快照、预留记录和全部标准流资源。 */
  PreparedLaunch(PreparedLaunch&&) noexcept = default;

  /** @brief 归还原预留记录，再接管快照、记录和全部标准流资源。 */
  auto operator=(PreparedLaunch&& other) noexcept -> PreparedLaunch& {
    if (this != &other) {
      release_record();
      snapshot_ = std::move(other.snapshot_);
      record_ = std::move(other.record_);
      pipes_ = std::move(other.pipes_);
      child_handles_ = std::move(other.child_handles_);
      std_handles_ = std::move(other.std_handles_);
    }
    return *this;
  }

  /** @brief 归还尚未使用的预留记录，未交付的原生资源由成员通过 RAII 释放。 */
  ~PreparedLaunch();

  /** @brief 借用待启动对象固定的命令快照。 */
  auto snapshot() const noexcept -> const CommandSnapshot& { return snapshot_; }

  /** @brief 借用准备阶段已经预留的进程记录。 */
  auto record() const noexcept -> const std::shared_ptr<ProcessState>& { return record_; }

  /** @brief 原生进程创建成功后，关闭父进程多余的子端副本，避免因自身持有管道而延迟 EOF。 */
  void close_child_ends() noexcept {
    for (auto& pipe : pipes_)
      if (pipe)
        pipe->close_child();
    for (auto& handle : child_handles_)
      handle.reset();
  }

  /** @brief 若 stdin 配置为管道，则将父写端交给公共包装。 */
  auto take_parent_stdin() -> expected<std::optional<ChildStdin>> { return take<ChildStdin>(0); }

  /** @brief 若 stdout 配置为管道，则将父读端交给公共包装。 */
  auto take_parent_stdout() -> expected<std::optional<ChildStdout>> { return take<ChildStdout>(1); }

  /** @brief 若 stderr 配置为管道，则将父读端交给公共包装。 */
  auto take_parent_stderr() -> expected<std::optional<ChildStderr>> { return take<ChildStderr>(2); }

 private:
  /** @brief 只归还尚未创建原生进程的预留记录，已启动记录留给服务回收。 */
  void release_record() noexcept;
  /** @brief 允许准备阶段填充快照、预留记录及标准流资源。 */
  friend auto prepare_launch(CommandSnapshot) -> expected<PreparedLaunch>;
  /** @brief 允许平台启动实现在不可分割的操作中填写预留记录的原生标识。 */
  friend auto launch_native(PreparedLaunch&) -> expected<NativeProcess>;

  /** @brief 接管命令快照和预留记录，后续原生资源由该对象回滚。 */
  PreparedLaunch(CommandSnapshot snapshot, std::shared_ptr<ProcessState> record)
      : snapshot_(std::move(snapshot)), record_(std::move(record)) {}

  /** @brief 移出指定标准管道的父进程端。 */
  template <class T>
  auto take(std::size_t index) -> expected<std::optional<T>> {
    if (!pipes_[index])
      return std::optional<T>{};
    auto pipe = pipes_[index]->take_parent(snapshot_.context());
    if (!pipe)
      return std::unexpected{pipe.error()};
    pipes_[index].reset();
    return std::optional<T>{std::in_place, std::move(*pipe)};
  }

  CommandSnapshot snapshot_; /**< CreateProcessW 使用的固定启动输入。 */
  std::shared_ptr<ProcessState>
      record_; /**< 预留或已创建进程的记录，地址保持不变，并持有进程句柄。 */
  std::array<std::optional<NativePipePair>, 3> pipes_; /**< 三路标准流的命名管道资源所有者。 */
  std::array<NativeHandle, 3> child_handles_; /**< 继承副本、空设备或普通文件的可继承句柄所有者。 */
  std::array<HANDLE, 3> std_handles_{}; /**< 提供给 STARTUPINFOEXW 及继承白名单的三路子端句柄。 */
};
}  // namespace faio::process::detail
#endif
