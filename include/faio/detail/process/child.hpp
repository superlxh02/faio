/**
 * @file faio/detail/process/child.hpp
 * @brief 独占公共子进程控制器与标准流所有权。
 */
#pragma once
#include "faio/detail/process/child_stdio.hpp"

namespace faio::process::detail {
/** @brief 以唯一代际令牌标识的进程记录，串行缓存退出结果。 */
class process_state;
/** @brief 整个程序共享的有界启动与回收服务，无需为每个子进程单独创建线程。 */
class process_service;
/** @brief 共享双流预算、退出结果和首个失败的捕获状态。 */
class capture_state;
/** @brief 持有记录锁以避免 PID 复用窗口，只对该记录对应且尚未退出的进程请求终止。 */
auto request_termination(const std::shared_ptr<process_state>&) -> expected<void>;
/** @brief 查询记录对应进程的缓存状态，发现终态后通知等待者和回收服务。 */
auto poll_exit(const std::shared_ptr<process_state>&) -> expected<std::optional<exit_status>>;
/** @brief 在已移出标准流的内部路径中异步等待进程退出。 */
auto wait_impl(std::shared_ptr<process_state>, io::operation_options)
    -> task<expected<exit_status>>;
/** @brief 在内部路径中请求终止并等待退出确认。 */
auto kill_impl(std::shared_ptr<process_state>, io::operation_options) -> task<expected<void>>;
/** @brief 将公共 Child 的析构策略交给共享回收服务。 */
void release_control(const std::shared_ptr<process_state>&, bool) noexcept;
}  // namespace faio::process::detail

namespace faio::process {
/** @brief 独占公共控制权的子进程包装，原生退出记录由服务继续维护。 */
class child {
 public:
  /** @brief 禁止构造尚未关联原生进程的公共控制器。 */
  child() = delete;
  /** @brief 禁止复制子进程公共控制权和三路标准流，避免重复持有所有权或复制运行状态。 */
  child(const child&) = delete;
  /** @brief 子进程公共控制权和三路标准流不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const child&) -> child& = delete;

  /** @brief 接管进程记录及三路标准流，清空移动源的流包装。 */
  child(child&& other) noexcept
      : state_(std::move(other.state_)),
        input_(std::move(other.input_)),
        output_(std::move(other.output_)),
        error_(std::move(other.error_)),
        kill_on_drop_(other.kill_on_drop_) {
    other.input_.reset();
    other.output_.reset();
    other.error_.reset();
  }

  /** @brief 关闭原有标准流并释放原公共控制权，再接管新 Child；移动源失去控制权。 */
  auto operator=(child&& other) noexcept -> child& {
    if (this != &other) {
      release();
      state_ = std::move(other.state_);
      input_ = std::move(other.input_);
      output_ = std::move(other.output_);
      error_ = std::move(other.error_);
      kill_on_drop_ = other.kill_on_drop_;
      other.input_.reset();
      other.output_.reset();
      other.error_.reset();
    }
    return *this;
  }

  /** @brief 请求关闭仍持有的标准流，并按 kill_on_drop 策略交还公共控制权。 */
  ~child() { release(); }

  /** @brief 返回尚未确认退出的进程 ID；无控制权或终态时为空。 */
  auto id() const noexcept -> std::optional<process_id>;

  /** @brief 借用仍打开的标准输入写端，所有权仍归 Child。 */
  auto stdin_stream() noexcept -> child_stdin* {
    return input_ && input_->is_open() ? &*input_ : nullptr;
  }

  /** @brief 借用仍打开的标准输出读端，所有权仍归 Child。 */
  auto stdout_stream() noexcept -> child_stdout* {
    return output_ && output_->is_open() ? &*output_ : nullptr;
  }

  /** @brief 借用仍打开的标准错误读端，所有权仍归 Child。 */
  auto stderr_stream() noexcept -> child_stderr* {
    return error_ && error_->is_open() ? &*error_ : nullptr;
  }

  /** @brief 借用仍打开的标准输入写端，所有权仍归 Child。 */
  auto stdin_stream() const noexcept -> const child_stdin* {
    return input_ && input_->is_open() ? &*input_ : nullptr;
  }

  /** @brief 借用仍打开的标准输出读端，所有权仍归 Child。 */
  auto stdout_stream() const noexcept -> const child_stdout* {
    return output_ && output_->is_open() ? &*output_ : nullptr;
  }

  /** @brief 借用仍打开的标准错误读端，所有权仍归 Child。 */
  auto stderr_stream() const noexcept -> const child_stderr* {
    return error_ && error_->is_open() ? &*error_ : nullptr;
  }

  /** @brief 一次性移出标准输入写端，后续 wait 不再替调用方关闭该端。 */
  auto take_stdin() -> std::optional<child_stdin> { return std::exchange(input_, {}); }

  /** @brief 一次性移出标准输出读端。 */
  auto take_stdout() -> std::optional<child_stdout> { return std::exchange(output_, {}); }

  /** @brief 一次性移出标准错误读端。 */
  auto take_stderr() -> std::optional<child_stderr> { return std::exchange(error_, {}); }

  /** @brief 同步请求强制终止，不等待子进程真正退出。 */
  auto start_kill() -> expected<void> { return detail::request_termination(state_); }

  /**
   * @brief 请求强制终止并异步等待退出确认。
   * @param options 本次终止等待的截止时间及显式取消令牌。
   * @return 已确认退出时成功；等待取消不会撤回已经发送的终止请求。
   */
  auto kill(io::operation_options options = {}) -> task<expected<void>>;

  /** @brief 非阻塞查询并缓存退出结果，不消耗后续 wait 可见的结果。 */
  auto try_wait() -> expected<std::optional<exit_status>> { return detail::poll_exit(state_); }

  /**
   * @brief 关闭仍持有的 stdin 并等待进程退出；取消后可再次等待。
   * @param options 本次退出等待的截止时间及显式取消令牌。
   * @return 可重复观察的退出状态；取消或超时只结束本次等待。
   */
  auto wait(io::operation_options options = {}) -> task<expected<exit_status>>;
  /**
   * @brief 验证资源预算后接管 Child，并发等待进程退出及两路输出到达 EOF。
   * @param options 捕获预算及整个等待过程的截止时间和取消令牌。
   * @return 完整输出或保留输出前缀的错误；无效预算不会消耗 Child。
   */
  auto wait_with_output(capture_options options = {}) && -> task<expected<output>>;
#if defined(_WIN32)
  /** @brief 借用仍受进程记录管理的 Windows 进程句柄。 */
  auto raw_handle() const noexcept -> void*;
#endif
  /** @brief 由内部工厂一次性交付进程记录和三路标准流所有权。 */
  child(std::shared_ptr<detail::process_state> state,
        bool kill_on_drop,
        std::optional<child_stdin> input,
        std::optional<child_stdout> output,
        std::optional<child_stderr> error)
      : state_(std::move(state)),
        input_(std::move(input)),
        output_(std::move(output)),
        error_(std::move(error)),
        kill_on_drop_(kill_on_drop) {}

 private:
  /** @brief 允许捕获状态接管 Child，并持续管理三个标准流。 */
  friend class detail::capture_state;

  /** @brief 释放并请求关闭仍持有的标准流，再按析构策略交还公共进程控制权。 */
  void release() noexcept {
    input_.reset();
    output_.reset();
    error_.reset();
    if (state_)
      detail::release_control(std::exchange(state_, {}), kill_on_drop_);
  }

  std::shared_ptr<detail::process_state> state_; /**< 公共控制器与已创建任务共享的进程记录。 */
  std::optional<child_stdin> input_;             /**< 尚未被 take_stdin 移出的父写端。 */
  std::optional<child_stdout> output_;           /**< 尚未被 take_stdout 移出的父读端。 */
  std::optional<child_stderr> error_;            /**< 尚未被 take_stderr 移出的父读端。 */
  bool kill_on_drop_{}; /**< 控制器析构时是否请求终止，回收责任始终由服务保留。 */
};
}  // namespace faio::process
