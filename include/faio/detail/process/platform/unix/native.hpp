/**
 * @file faio/detail/process/platform/unix/native.hpp
 * @brief Unix 文件描述符、管道对和待启动资源的独占所有权。
 */
#pragma once
#if !defined(_WIN32)
#include "faio/detail/process/command_snapshot.hpp"
#include "faio/detail/process/child.hpp"
#include "faio/detail/process/process_service.hpp"
#include <array>
#include <fcntl.h>
#include <unistd.h>

namespace faio::process::detail {
/** @brief 独占持有 Unix 文件描述符，并通过 close 释放。 */
class native_fd_type {
 public:
  /** @brief 接管 Unix 描述符；默认值表示尚无待关闭资源。 */
  native_fd_type() = default;

  /** @brief 接管 Unix 描述符；默认值表示尚无待关闭资源。 */
  explicit native_fd_type(int value) : value_(value) {}

  /** @brief 禁止复制 Unix 描述符，避免重复持有关闭责任。 */
  native_fd_type(const native_fd_type&) = delete;
  /** @brief Unix 描述符不支持复制赋值，以免破坏已有所有权或关联。 */
  native_fd_type& operator=(const native_fd_type&) = delete;

  /** @brief 接管描述符并将移动源置为负值。 */
  native_fd_type(native_fd_type&& other) noexcept : value_(std::exchange(other.value_, -1)) {}

  /** @brief 先关闭原描述符，再接管新描述符；移动源恢复为负值。 */
  native_fd_type& operator=(native_fd_type&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }

  /** @brief 关闭仍拥有的 Unix 描述符。 */
  ~native_fd_type() { reset(); }

  /** @brief 借用当前原生资源，不移交关闭责任。 */
  auto get() const noexcept -> int { return value_; }

  /** @brief 移出原生资源并清空所有者，调用方接管关闭责任。 */
  auto release() noexcept -> int { return std::exchange(value_, -1); }

  /** @brief 先释放原有原生资源，再接管新的资源值。 */
  void reset(int value = -1) noexcept {
    if (value_ >= 0)
      ::close(value_);
    value_ = value;
  }

 private:
  int value_{-1}; /**< 独占文件描述符，负值表示无资源。 */
};

/** @brief 将零至二号描述符提升到标准流范围之外，并保留 CLOEXEC。 */
inline auto lift_fd(int fd) -> expected<native_fd_type> {
  native_fd_type owner{fd};
  if (fd < 0)
    return std::unexpected{make_error(errno)};
  if (fd >= 3)
    return owner;
  const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
  if (moved < 0)
    return std::unexpected{make_error(errno)};
  return native_fd_type{moved};
}

/** @brief 串行创建标准流副本和管道，避免并发启动时错误继承资源。 */
inline std::mutex& spawn_creation_mutex() {
  static std::mutex mutex;
  return mutex;
}

/** @brief 同时拥有父端异步句柄和子端阻塞句柄的管道对。 */
class native_pipe_pair {
 public:
  /** @brief 禁止复制父子两端管道，避免重复持有所有权或复制运行状态。 */
  native_pipe_pair(const native_pipe_pair&) = delete;
  /** @brief 父子两端管道不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const native_pipe_pair&) -> native_pipe_pair& = delete;
  /** @brief 移交父端、子端和方向，两端资源的关闭责任一并转移。 */
  native_pipe_pair(native_pipe_pair&&) noexcept = default;
  /** @brief 关闭本对象原有管道端，再接管父端、子端及方向。 */
  auto operator=(native_pipe_pair&&) noexcept -> native_pipe_pair& = default;
  /** @brief 由两端资源的所有者关闭尚未移交的父端和子端。 */
  ~native_pipe_pair() = default;

  /** @brief 创建方向固定的父子管道对，父端异步、子端阻塞且仅子端可继承。 */
  static auto create(bool parent_writer) -> expected<native_pipe_pair> {
    int fds[2]{-1, -1};
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) < 0)
      return std::unexpected{make_error(errno)};
#else
    if (::pipe(fds) < 0)
      return std::unexpected{make_error(errno)};
    if (::fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
      const int e = errno;
      ::close(fds[0]);
      ::close(fds[1]);
      return std::unexpected{make_error(e)};
    }
#endif
    native_fd_type a{fds[0]}, b{fds[1]};
    auto left = lift_fd(a.release());
    if (!left)
      return std::unexpected{left.error()};
    auto right = lift_fd(b.release());
    if (!right)
      return std::unexpected{right.error()};
    native_pipe_pair result{parent_writer};
    result.parent_ = parent_writer ? std::move(*right) : std::move(*left);
    result.child_ = parent_writer ? std::move(*left) : std::move(*right);
    const auto flags = ::fcntl(result.parent_.get(), F_GETFL);
    if (flags < 0 || ::fcntl(result.parent_.get(), F_SETFL, flags | O_NONBLOCK) < 0)
      return std::unexpected{make_error(errno)};
    return result;
  }

  /** @brief 借用供原生子进程继承的管道端。 */
  auto child_native() const noexcept -> io::detail::native_descriptor { return child_.get(); }

  /** @brief 将父端交给指定 IO 域，仅在成功时移交关闭责任。 */
  auto take_parent(io::io_context context) -> expected<std::shared_ptr<pipe_state>> {
    auto result = pipe_state::adopt(std::move(context), parent_.get(), writer_);
    if (result)
      (void)parent_.release();
    return result;
  }

  /** @brief 关闭父进程持有的子端副本。 */
  void close_child() noexcept { child_.reset(); }

  /** @brief 关闭尚未交给 IO 域的父端副本。 */
  void close_parent() noexcept { parent_.reset(); }

 private:
  /** @brief 保存父端方向，管道资源由创建步骤逐项填入。 */
  explicit native_pipe_pair(bool writer) : writer_(writer) {}

  native_fd_type parent_; /**< 待交给 IO 域的父端非阻塞描述符。 */
  native_fd_type child_;  /**< 只供子进程标准流使用的阻塞描述符。 */
  bool writer_;           /**< 父端是否负责写入子进程 stdin。 */
};

/** @brief 持有尚未交给监视服务的原生进程责任，析构时清理启动失败留下的资源。 */
class native_process {
 public:
  /** @brief 禁止复制尚未交接的进程清理责任，避免重复持有所有权或复制运行状态。 */
  native_process(const native_process&) = delete;
  /** @brief 尚未交接的进程清理责任不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const native_process&) -> native_process& = delete;

  /** @brief 转交尚未交接的进程记录和清理责任，将移动源 ID 归零。 */
  native_process(native_process&& other) noexcept
      : record_(std::move(other.record_)), pid_(std::exchange(other.pid_, 0)) {}

  /** @brief 清理原有尚未交接的进程，再接管新记录及进程 ID。 */
  auto operator=(native_process&& other) noexcept -> native_process& {
    if (this != &other) {
      cleanup();
      record_ = std::move(other.record_);
      pid_ = std::exchange(other.pid_, 0);
    }
    return *this;
  }

  /** @brief 终止尚未由服务接管的原生进程，并保留服务的回收路径。 */
  ~native_process() { cleanup(); }

  /** @brief 返回尚未交接的原生进程 ID，清理或服务交接后为零。 */
  auto id() const noexcept -> process_id { return pid_; }

 private:
  /** @brief 允许共享服务推进启动状态、登记监视资源并归还记录槽。 */
  friend class process_service;
  /** @brief 允许平台启动实现在不可分割的操作中填写预留记录的原生标识。 */
  friend auto launch_native(/** 持有待启动进程的参数、管道和预留的监视资源。 */
                            class prepared_launch&) -> expected<native_process>;

  /** @brief 接管尚未完成服务登记的原生进程清理责任。 */
  native_process(std::shared_ptr<process_state> record, process_id pid)
      : record_(std::move(record)), pid_(pid) {}

  /** @brief 对未完成服务交接的进程请求终止，并恢复服务的回收路径。 */
  void cleanup() noexcept;
  std::shared_ptr<process_state> record_; /**< 预留或已创建进程的记录，地址保持不变。 */
  process_id pid_{};                      /**< 尚未完成服务交接的原生进程 ID。 */
};

/** @brief 预留进程记录并拥有全部启动前资源，失败时自动归还。 */
class prepared_launch {
 public:
  /** @brief 禁止复制待启动快照、记录及标准流资源，避免重复持有所有权或复制运行状态。 */
  prepared_launch(const prepared_launch&) = delete;
  /** @brief 待启动快照、记录及标准流资源不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const prepared_launch&) -> prepared_launch& = delete;
  /** @brief 转交启动快照、预留记录和全部标准流资源。 */
  prepared_launch(prepared_launch&&) noexcept = default;

  /** @brief 归还原预留记录，再接管快照、记录和全部标准流资源。 */
  auto operator=(prepared_launch&& other) noexcept -> prepared_launch& {
    if (this != &other) {
      release_record();
      snapshot_ = std::move(other.snapshot_);
      record_ = std::move(other.record_);
      pipes_ = std::move(other.pipes_);
      files_ = std::move(other.files_);
      child_fds_ = std::move(other.child_fds_);
    }
    return *this;
  }

  /** @brief 归还尚未使用的预留记录，未交付的原生资源由成员通过 RAII 释放。 */
  ~prepared_launch();

  /** @brief 借用待启动对象固定的命令快照。 */
  auto snapshot() const noexcept -> const command_snapshot& { return snapshot_; }

  /** @brief 借用准备阶段已经预留的进程记录。 */
  auto record() const noexcept -> const std::shared_ptr<process_state>& { return record_; }

  /** @brief 原生进程创建成功后，关闭父进程多余的子端副本，避免因自身持有管道而延迟 EOF。 */
  void close_child_ends() noexcept {
    for (auto& pair : pipes_)
      if (pair)
        pair->close_child();
    for (auto& fd : files_)
      fd.reset();
  }

  /** @brief 若 stdin 配置为管道，则将父写端交给公共包装。 */
  auto take_parent_stdin() -> expected<std::optional<child_stdin>> { return take<child_stdin>(0); }

  /** @brief 若 stdout 配置为管道，则将父读端交给公共包装。 */
  auto take_parent_stdout() -> expected<std::optional<child_stdout>> {
    return take<child_stdout>(1);
  }

  /** @brief 若 stderr 配置为管道，则将父读端交给公共包装。 */
  auto take_parent_stderr() -> expected<std::optional<child_stderr>> {
    return take<child_stderr>(2);
  }

 private:
  /** @brief 只归还尚未创建原生进程的预留记录，已启动记录留给服务回收。 */
  void release_record() noexcept;
  /** @brief 允许准备阶段填充快照、预留记录及标准流资源。 */
  friend auto prepare_launch(command_snapshot) -> expected<prepared_launch>;
  /** @brief 允许平台启动实现在不可分割的操作中填写预留记录的原生标识。 */
  friend auto launch_native(prepared_launch&) -> expected<native_process>;

  /** @brief 接管命令快照和预留记录，后续原生资源由该对象回滚。 */
  prepared_launch(command_snapshot snapshot, std::shared_ptr<process_state> record)
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

  command_snapshot snapshot_;             /**< 原生创建使用的调用时固定输入。 */
  std::shared_ptr<process_state> record_; /**< 预留或已创建进程的记录，地址保持不变。 */
  std::array<std::optional<native_pipe_pair>, 3> pipes_; /**< 三路标准流中需要管道的资源所有者。 */
  std::array<native_fd_type, 3> files_; /**< 继承副本、空设备或普通文件的子端所有者。 */
  std::array<int, 3> child_fds_{
      {-1, -1, -1}}; /**< 按 stdin/stdout/stderr 顺序供 posix_spawn 绑定的描述符。 */
};
}  // namespace faio::process::detail
#endif
