/**
 * @file faio/detail/process/capture.hpp
 * @brief 并发读取双输出并保留有界前缀的捕获状态机。
 */
#pragma once
#include "faio/detail/process/child_operations.hpp"
#include "faio/detail/io/stream/operation.hpp"
#include "faio/detail/coroutine/composition.hpp"
#include <algorithm>

namespace faio::process::detail {
/** @brief 捕获状态，保存两路输出共享的资源预算、退出结果和首个失败。 */
class capture_state : public std::enable_shared_from_this<capture_state> {
 public:
  /** @brief 禁止复制输出捕获状态，避免重复持有所有权或复制运行状态。 */
  capture_state(const capture_state&) = delete;
  /** @brief 输出捕获状态不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const capture_state&) -> capture_state& = delete;
  /** @brief 输出捕获状态不能移动；回调和协程依赖其固定地址。 */
  capture_state(capture_state&&) = delete;
  /** @brief 输出捕获状态不支持移动赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(capture_state&&) -> capture_state& = delete;
  /** @brief 释放输出缓冲和 Child，沿用 Child 的析构终止策略。 */
  ~capture_state() = default;

  /** @brief 接管 Child 并分配稳定捕获状态，分配失败返回 ENOMEM。 */
  static auto create(::faio::process::child child, capture_options options)
      -> expected<std::shared_ptr<capture_state>> {
    try {
      struct factory final : capture_state {
        /** @brief 原样转发构造参数，通过 make_shared 在一次分配中创建地址固定的状态。 */
        factory(::faio::process::child owned, capture_options capture)
            : capture_state(std::move(owned), capture) {}
      };

      return std::make_shared<factory>(std::move(child), options);
    } catch (const std::bad_alloc&) {
      return std::unexpected{make_error(ENOMEM)};
    }
  }

  /** @brief 在 stdout 独立预算和合计预算内追加已经读取的字节。 */
  auto append_stdout(std::span<const char> bytes) -> expected<void> { return append(0, bytes); }

  /** @brief 在 stderr 独立预算和合计预算内追加已经读取的字节。 */
  auto append_stderr(std::span<const char> bytes) -> expected<void> { return append(1, bytes); }

  /** @brief 在结果锁下记录 stdout 已经读到 EOF。 */
  void set_stdout_eof() noexcept {
    std::lock_guard lock(mutex_);
    eof_[0] = true;
  }

  /** @brief 在结果锁下记录 stderr 已经读到 EOF。 */
  void set_stderr_eof() noexcept {
    std::lock_guard lock(mutex_);
    eof_[1] = true;
  }

  /** @brief 缓存退出状态；等待失败时，转入处理首个失败的协调流程。 */
  void set_exit(expected<exit_status> result) {
    if (result) {
      std::lock_guard lock(mutex_);
      exit_ = *result;
    } else
      fail(result.error());
  }

  /** @brief 只保存首个失败，必要时请求终止子进程，并停止捕获分支。 */
  auto fail(error error) -> bool {
    bool kill{};
    {
      std::lock_guard lock(mutex_);
      if (failure_)
        return false;
      kill = error.value() != ECANCELED && error.value() != ETIMEDOUT;
      failure_ = std::move(error);
    }
    if (kill)
      (void)child_.start_kill();
    stop_.request_stop();
    return true;
  }

  /** @brief 返回用于停止三个捕获分支的共享取消令牌。 */
  auto stop_token() const noexcept -> std::stop_token { return stop_.get_token(); }

  /** @brief 仅交付一次完整结果；失败时把已捕获前缀和截断信息放入 Error 上下文。 */
  auto take_output() -> expected<output> {
    std::lock_guard lock(mutex_);
    if (taken_)
      return std::unexpected{make_error(EBADF)};
    taken_ = true;
    if (failure_) {
      try {
        auto details = std::make_shared<error_context>();
        if (failure_->context())
          *details = *failure_->context();
        details->process_id = pid_;
        if (exit_) {
          details->process_exit_code = exit_->code();
#if !defined(_WIN32)
          details->process_signal = exit_->signal();
#endif
        }
        details->captured_stdout = std::move(output_[0]);
        details->captured_stderr = std::move(output_[1]);
        details->stdout_truncated = truncated_[0];
        details->stderr_truncated = truncated_[1];
        const auto size = details->captured_stdout.size() + details->captured_stderr.size();
        return std::unexpected{failure_->with_context(std::move(details)).with_progress(size)};
      } catch (const std::bad_alloc&) {
        return std::unexpected{make_error(ENOMEM)};
      }
    }
    if (!exit_ || !eof_[0] || !eof_[1])
      return std::unexpected{process_error(errc::OPERATION_IN_PROGRESS)};
    return output{*exit_, std::move(output_[0]), std::move(output_[1])};
  }

  /** @brief 在同一把锁下计算指定流及双流合计预算的剩余字节数。 */
  auto allowance(std::size_t index) -> std::size_t {
    std::lock_guard lock(mutex_);
    const auto individual = (index == 0 ? options_.max_stdout_bytes : options_.max_stderr_bytes)
                            - output_[index].size();
    const auto total = options_.max_total_bytes - output_[0].size() - output_[1].size();
    return std::min(individual, total);
  }

  /** @brief 借用捕获状态独占持有的 Child 控制器。 */
  ::faio::process::child& child() noexcept { return child_; }

  /** @brief 借用仅供指定读取分支使用的暂存缓冲。 */
  auto scratch(std::size_t index) -> std::span<char> { return scratch_[index]; }

 private:
  /** @brief 接管 Child，保存捕获预算并为双输出各自分配暂存缓冲。 */
  capture_state(::faio::process::child child, capture_options options)
      : child_(std::move(child)), options_(options), pid_(child_.id()) {
    scratch_[0].resize(options.read_chunk_bytes);
    scratch_[1].resize(options.read_chunk_bytes);
  }

  /** @brief 原子预留双流共享预算，保留预算内的前缀；超额时转入失败状态。 */
  auto append(std::size_t index, std::span<const char> bytes) -> expected<void> {
    try {
      bool over{};
      {
        std::lock_guard lock(mutex_);
        const auto individual = (index == 0 ? options_.max_stdout_bytes : options_.max_stderr_bytes)
                                - output_[index].size();
        const auto remaining = options_.max_total_bytes - output_[0].size() - output_[1].size();
        const auto count = std::min({bytes.size(), individual, remaining});
        const auto needed = output_[index].size() + count;
        if (needed > output_[index].capacity()) {
          const auto limit =
              std::min(index == 0 ? options_.max_stdout_bytes : options_.max_stderr_bytes,
                       options_.max_total_bytes);
          const auto old = output_[index].capacity();
          output_[index].reserve(std::max(needed, old + std::min(old, limit - old)));
        }
        output_[index].insert(output_[index].end(),
                              bytes.begin(),
                              bytes.begin() + static_cast<std::ptrdiff_t>(count));
        over = count < bytes.size();
        if (over)
          truncated_[index] = true;
      }
      if (over) {
        auto error = process_error(errc::OUTPUT_LIMIT_EXCEEDED);
        fail(error);
        return std::unexpected{std::move(error)};
      }
      return {};
    } catch (const std::bad_alloc&) {
      auto error = make_error(ENOMEM);
      fail(error);
      return std::unexpected{std::move(error)};
    }
  }

  ::faio::process::child child_;             /**< 捕获过程独占的子进程控制器及尚未移出的标准流。 */
  capture_options options_;                  /**< 固定的各输出预算、合计预算和读取块大小。 */
  std::optional<process_id> pid_;            /**< 捕获开始时的进程标识，供失败上下文诊断。 */
  std::mutex mutex_;                         /**< 保护双流共享预算、输出前缀及唯一失败。 */
  std::array<std::vector<char>, 2> output_;  /**< 按 stdout、stderr 顺序保存已接纳的输出前缀。 */
  std::array<std::vector<char>, 2> scratch_; /**< 两个读取分支各自独占的固定暂存缓冲。 */
  bool eof_[2]{};                            /**< 两个方向是否已经读到 EOF。 */
  bool truncated_[2]{};                      /**< 各方向是否因预算不足而只保留前缀。 */
  bool taken_{};                             /**< 确保 Output 或失败上下文只被移出一次。 */
  std::optional<exit_status> exit_;          /**< 退出等待分支已经确认的子进程终态。 */
  std::optional<error> failure_;             /**< 三个分支共享的首个失败原因。 */
  std::stop_source stop_;                    /**< 失败时停止另外两个分支并推动它们排空。 */
};

/** @brief 持续读取一个输出方向，预算耗尽后用一字节探针区分 EOF 与超限。 */
inline auto drain(std::shared_ptr<capture_state> state,
                  std::size_t index,
                  io::operation_options options) -> task<expected<void>> {
  auto* output = state->child().stdout_stream();
  auto* error = state->child().stderr_stream();
  if ((index == 0 && !output) || (index == 1 && !error)) {
    if (index == 0)
      state->set_stdout_eof();
    else
      state->set_stderr_eof();
    co_return expected<void>{};
  }
  for (;;) {
    auto bytes = state->scratch(index);
    bytes = bytes.first(std::min(bytes.size(), std::max<std::size_t>(1, state->allowance(index))));
    auto result =
        index == 0 ? co_await output->read(bytes, options) : co_await error->read(bytes, options);
    const auto count =
        result ? *result : std::min<std::size_t>(bytes.size(), result.error().progress());
    if (count) {
      auto appended = index == 0 ? state->append_stdout(bytes.first(count))
                                 : state->append_stderr(bytes.first(count));
      if (!appended)
        co_return std::unexpected{appended.error()};
    }
    if (!result) {
      state->fail(result.error());
      co_return std::unexpected{result.error()};
    }
    if (!count) {
      if (index == 0)
        state->set_stdout_eof();
      else
        state->set_stderr_eof();
      co_return expected<void>{};
    }
  }
}

/** @brief 启动 stdout 单读者分支，与 stderr 和退出等待并发执行。 */
inline auto drain_stdout(std::shared_ptr<capture_state> state, io::operation_options options)
    -> task<expected<void>> {
  return drain(std::move(state), 0, options);
}

/** @brief 启动 stderr 单读者分支，与 stdout 和退出等待并发执行。 */
inline auto drain_stderr(std::shared_ptr<capture_state> state, io::operation_options options)
    -> task<expected<void>> {
  return drain(std::move(state), 1, options);
}

/** @brief 等待子进程终态并把结果写入捕获状态。 */
inline auto capture_wait(std::shared_ptr<capture_state> state, io::operation_options options)
    -> task<expected<void>> {
  auto result = co_await state->child().wait(options);
  state->set_exit(result);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}

/** @brief 合并取消请求，关闭仍持有的 stdin，并等待退出、stdout 和 stderr 三个分支全部排空。 */
inline auto capture_impl(std::shared_ptr<capture_state> state, capture_options options)
    -> task<expected<output>> {
  auto parent = co_await this_coro::stop_token();
  if (auto checked = preflight(options.operation, parent); !checked) {
    state->fail(checked.error());
    co_return state->take_output();
  }
  if (auto input = state->child().take_stdin()) {
    auto closed = co_await input->close();
    if (!closed) {
      state->fail(closed.error());
      co_return state->take_output();
    }
  }
  auto cancel = [weak = std::weak_ptr{state}] {
    if (auto s = weak.lock())
      s->fail(make_error(ECANCELED));
  };
  std::stop_callback parent_stop{parent, cancel};
  std::stop_callback explicit_stop{options.operation.stop, cancel};
  auto children = [state, options]() mutable -> task<void> {
    (void)co_await join(capture_wait(state, options.operation),
                        drain_stdout(state, options.operation),
                        drain_stderr(state, options.operation));
  };
  co_await io::detail::bind_stop(children(), state->stop_token());
  co_return state->take_output();
}

/** @brief 将调用时发现的错误保存到未启动的捕获任务中。 */
inline auto failed_capture(error error) -> task<expected<output>> {
  co_return std::unexpected{std::move(error)};
}
}  // namespace faio::process::detail

namespace faio::process {
/** @brief 验证资源预算后接管 Child，并发等待进程退出及两路输出到达 EOF。 */
inline auto child::wait_with_output(capture_options options) && -> task<expected<output>> {
  if (auto valid = detail::validate_capture(options); !valid)
    return detail::failed_capture(valid.error());
  auto state = detail::capture_state::create(std::move(*this), options);
  if (!state)
    return detail::failed_capture(state.error());
  return detail::capture_impl(std::move(*state), options);
}
}  // namespace faio::process
