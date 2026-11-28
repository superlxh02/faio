/**
 * @file faio/detail/process/launch_operation.hpp
 * @brief 启动请求排队、交付和取消后的清理状态机。
 */
#pragma once
#include "faio/detail/process/command_snapshot.hpp"
#include "faio/detail/process/exit_wait_operation.hpp"
#include "faio/detail/io/stream/operation.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/process/execution_adapter/operation_scope.hpp"
#endif
#if defined(_WIN32)
#include "faio/detail/process/platform/windows/spawn.hpp"
#else
#include "faio/detail/process/platform/unix/spawn.hpp"
#endif
namespace faio::process::detail {
/** @brief 准备原生资源、创建进程并交给服务，只有全部接管成功才交付 Child。 */
inline auto spawn_sync(command_snapshot snapshot) -> expected<child> {
  try {
    if (snapshot.context().stopped())
      return std::unexpected{process_error(errc::RUNTIME_SHUTDOWN)};
    auto prepared = prepare_launch(std::move(snapshot));
    if (!prepared)
      return std::unexpected{prepared.error()};
    auto native = launch_native(*prepared);
    if (!native)
      return std::unexpected{native.error()};
    auto service = process_service::instance();
    if (!service)
      return std::unexpected{service.error()};
    auto watched = (*service)->watch(prepared->record(), std::move(*native));
    if (!watched)
      return std::unexpected{watched.error()};

    struct delivery_guard {
      std::shared_ptr<process_state> state; /**< 尚未交付公共控制权的原生进程记录。 */

      /** @brief 即使登记运行时关联或接管标准流时抛出异常，也不留下无人控制的运行中进程。 */
      ~delivery_guard() {
        if (state)
          (void)request_termination(state);
      }
    } pending{prepared->record()};

#if defined(FAIO_PROCESS_TEST_BEFORE_RUNTIME_ATTACHMENT)
    FAIO_PROCESS_TEST_BEFORE_RUNTIME_ATTACHMENT(prepared->record());
#endif
    prepared->record()->attach_runtime(prepared->snapshot().context());
    auto input = prepared->take_parent_stdin();
    if (!input)
      return std::unexpected{input.error()};
    auto output = prepared->take_parent_stdout();
    if (!output)
      return std::unexpected{output.error()};
    auto error = prepared->take_parent_stderr();
    if (!error)
      return std::unexpected{error.error()};
    if (prepared->snapshot().context().stopped())
      return std::unexpected{process_error(errc::RUNTIME_SHUTDOWN)};
    child child{prepared->record(),
                prepared->snapshot().command().get_kill_on_drop(),
                std::move(*input),
                std::move(*output),
                std::move(*error)};
    pending.state.reset();
    return child;
  } catch (const std::bad_alloc&) {
    return std::unexpected{make_error(ENOMEM)};
  } catch (const std::system_error& error) {
    return std::unexpected{make_error(error.code().value())};
  }
}
/** @brief 启动请求从创建到交付或放弃的生命周期阶段。 */
enum class launch_phase {
  CREATED,    /**< 已固定请求输入，尚未进入启动队列。 */
  QUEUED,     /**< 已占用有界启动队列位置。 */
  LAUNCHING,  /**< 启动线程已接纳，原生创建可能正在进行。 */
  REGISTERED, /**< 为原生进程监视接管阶段保留的状态值。 */
  DELIVERED,  /**< 唯一结果已移交给等待者，不再接受启动取消。 */
  ABANDONED,  /**< 取消胜出，未交付的新进程已转入终止清理。 */
  FINISHED    /**< 执行结果已保存，等待唯一调用方领取。 */
};

/** @brief 在调用协程与启动线程之间传递请求和唯一结果的状态，地址保持不变。 */
class launch_operation : public std::enable_shared_from_this<launch_operation> {
 public:
  /** @brief 禁止复制启动操作状态，避免重复持有所有权或复制运行状态。 */
  launch_operation(const launch_operation&) = delete;
  /** @brief 启动操作状态不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const launch_operation&) -> launch_operation& = delete;
  /** @brief 启动操作状态不能移动；回调和协程依赖其固定地址。 */
  launch_operation(launch_operation&&) = delete;
  /** @brief 启动操作状态不支持移动赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(launch_operation&&) -> launch_operation& = delete;

  /** @brief 未交付的成功启动结果仍需请求终止，避免遗留无人控制的子进程。 */
  ~launch_operation() {
    if (result_ && *result_ && phase_ != launch_phase::DELIVERED)
      (void)(**result_).start_kill();
  }

  /** @brief 分配地址固定的启动状态，保存任务图租约；分配失败时返回错误。 */
  static auto create(command_snapshot snapshot,
                     io::operation_options options,
                     std::stop_token parent) -> expected<std::shared_ptr<launch_operation>> {
    try {
      struct factory final : launch_operation {
        /** @brief 原样转发构造参数，通过 make_shared 在一次分配中创建地址固定的状态。 */
        factory(command_snapshot owned, io::operation_options operation, std::stop_token token)
            : launch_operation(std::move(owned), operation, token) {}
      };

      return std::make_shared<factory>(std::move(snapshot), options, parent);
    } catch (const std::bad_alloc&) {
      return std::unexpected{make_error(ENOMEM)};
    }
  }

  /** @brief 在结果锁下读取当前启动阶段。 */
  auto phase() const noexcept -> launch_phase {
    std::lock_guard lock(mutex_);
    return phase_;
  }

  /** @brief 保存本次请求的取消原因，并推动尚未交付的工作结束。 */
  void request_stop(error error) noexcept {
    {
      std::lock_guard lock(mutex_);
      if (phase_ == launch_phase::DELIVERED || phase_ == launch_phase::FINISHED)
        return;
      cancel_ = std::move(error);
    }
    if (auto service = process_service::instance())
      (*service)->cancel_queued(shared_from_this());
  }

  /** @brief 由启动服务执行原生创建；若取消先完成，则终止尚未交付的子进程。 */
  void execute() noexcept {
    bool cancelled{};
    {
      std::lock_guard lock(mutex_);
      if (phase_ == launch_phase::LAUNCHING || phase_ == launch_phase::FINISHED
          || phase_ == launch_phase::DELIVERED)
        return;
      phase_ = launch_phase::LAUNCHING;
      cancelled = cancel_.has_value();
    }
    expected<child> result = std::unexpected{make_error(ECANCELED)};
    if (!cancelled) {
      try {
        result = spawn_sync(std::move(*snapshot_));
      } catch (...) {
        result = std::unexpected{make_error(ENOMEM)};
      }
    }
    snapshot_.reset();
    {
      std::lock_guard lock(mutex_);
      if (cancel_) {
        if (result)
          (void)result->start_kill();
        result_ = std::unexpected{*cancel_};
        phase_ = launch_phase::ABANDONED;
      } else {
        result_.emplace(std::move(result));
        phase_ = launch_phase::FINISHED;
      }
    }
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    auto publisher = execution_scope_.start_publisher();
#endif
    completion_.notify();
  }

  /** @brief 安装取消回调并等待启动线程交付结果，取消也须等待清理完成。 */
  auto wait() -> task<expected<void>> {
    auto keep = shared_from_this();
    auto stop = [weak = std::weak_ptr{keep}] {
      if (auto self = weak.lock())
        self->request_stop(make_error(ECANCELED));
    };
    std::stop_callback parent{parent_, stop};
    std::stop_callback explicit_stop{options_.stop, stop};
    auto service = process_service::instance();
    if (!service)
      co_return std::unexpected{service.error()};
    auto enqueued = (*service)->enqueue_launch(keep);
    if (!enqueued)
      co_return std::unexpected{enqueued.error()};
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    auto lease = execution_scope_.acquire_child();
#endif
    co_await completion_.wait();
    co_return expected<void>{};
  }

  /** @brief 只允许一次移出已完成结果，并标记控制权已经交付。 */
  auto take_result() -> expected<child> {
    std::lock_guard lock(mutex_);
    if (taken_)
      return std::unexpected{make_error(EBADF)};
    if (!result_)
      return std::unexpected{process_error(errc::OPERATION_IN_PROGRESS)};
    taken_ = true;
    phase_ = launch_phase::DELIVERED;
    return std::move(*result_);
  }

 private:
  /** @brief 允许共享服务推进启动状态、登记监视资源并归还记录槽。 */
  friend class process_service;

  /** @brief 保存启动输入并取得当前外部任务图的完成租约。 */
  launch_operation(command_snapshot snapshot, io::operation_options options, std::stop_token parent)
      : snapshot_(std::move(snapshot)), options_(options), parent_(parent) {
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
    execution_scope_.capture();
#endif
  }

  mutable std::mutex mutex_;                    /**< 串行保护启动阶段、取消原因和唯一结果。 */
  launch_phase phase_{launch_phase::CREATED};   /**< 请求当前所在的启动交付阶段。 */
  std::optional<command_snapshot> snapshot_;    /**< 执行前拥有的启动输入，原生创建结束后释放。 */
  io::operation_options options_;               /**< 调用方提供的显式取消和整体截止时间。 */
  std::stop_token parent_;                      /**< 本次启动所继承的取消令牌。 */
  std::optional<error> cancel_;                 /**< 交付前观察到的取消或到期原因。 */
  std::optional<expected<child>> result_;       /**< 原生创建已排空后保存的成功 Child 或错误。 */
  bool taken_{};                                /**< 保证结果只能移交一次。 */
  ::faio::detail::completion_event completion_; /**< 启动线程完成结果发布后的唤醒事件。 */
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
  execution_adapter::operation_scope execution_scope_;
#endif
};

/** @brief 将启动请求加入有界队列；队列已满时立即拒绝。 */
inline auto process_service::enqueue_launch(std::shared_ptr<launch_operation> operation)
    -> expected<void> {
  try {
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return std::unexpected{process_error(errc::RUNTIME_SHUTDOWN)};
      if (launches_.size() >= 256)
        return std::unexpected{process_error(errc::LAUNCH_QUEUE_FULL)};
      {
        std::lock_guard op_lock(operation->mutex_);
        operation->phase_ = launch_phase::QUEUED;
      }
      launches_.push_back(std::move(operation));
    }
    cv_.notify_one();
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{make_error(ENOMEM)};
  }
}

/** @brief 从启动队列移除尚未执行的请求，再发布其取消结果。 */
inline void process_service::cancel_queued(
    const std::shared_ptr<launch_operation>& operation) noexcept {
  bool removed{};
  {
    std::lock_guard lock(mutex_);
    for (auto it = launches_.begin(); it != launches_.end(); ++it)
      if (*it == operation) {
        launches_.erase(it);
        removed = true;
        break;
      }
  }
  if (removed)
    operation->execute();
}

/** @brief 由固定启动线程串行执行原生创建并协调 Windows 退出清理。 */
inline void process_service::launch_loop() noexcept {
  for (;;) {
#if defined(_WIN32)
    const auto revision = revision_.load();
    sweep();
    const auto deadline = next_deadline();
#endif
    std::shared_ptr<launch_operation> operation;
    {
      std::unique_lock lock(mutex_);
#if defined(_WIN32)
      if (deadline)
        cv_.wait_until(lock, *deadline, [&] {
          return stopping_ || !launches_.empty() || revision_.load() != revision;
        });
      else
        cv_.wait(lock,
                 [&] { return stopping_ || !launches_.empty() || revision_.load() != revision; });
#else
      cv_.wait(lock, [&] { return stopping_ || !launches_.empty(); });
#endif
      if (!launches_.empty()) {
        operation = std::move(launches_.front());
        launches_.pop_front();
      } else if (stopping_)
        break;
    }
    if (operation)
      operation->execute();
  }
}

/** @brief 停止固定服务线程并等待其结束，再排空平台退出通知的清理工作。 */
inline process_service::~process_service() {
  stopping_.store(true);
  notify_control_change();
  if (launch_.joinable())
    launch_.join();
#if !defined(_WIN32)
  if (monitor_.joinable())
    monitor_.join();
  sweep();
  ::close(poller_);
#if defined(__linux__)
  ::close(control_);
#endif
#else
  sweep();
  for (auto& state : records_)
    if (state) {
      PTP_WAIT wait{};
      {
        std::lock_guard lock(state->mutex_);
        wait = std::exchange(state->native_wait_, nullptr);
      }
      if (wait) {
        ::SetThreadpoolWait(wait, nullptr, nullptr);
        ::WaitForThreadpoolWaitCallbacks(wait, TRUE);
        ::CloseThreadpoolWait(wait);
      }
    }
#endif
}

/** @brief 启动操作排空后，决定是否向调用方交付新建的 Child。 */
inline auto spawn_async_impl(command_snapshot snapshot, io::operation_options options)
    -> task<expected<child>> {
  auto body = [snapshot = std::move(snapshot),
               options](io::detail::stream_operation_state<child>& result) mutable -> task<void> {
    auto operation =
        launch_operation::create(std::move(snapshot), options, co_await this_coro::stop_token());
    if (!operation) {
      result.try_commit(
          [&]() noexcept -> expected<child> { return std::unexpected{operation.error()}; });
      co_return;
    }
    auto waited = co_await (*operation)->wait();
    if (!waited) {
      result.try_commit(
          [&]() noexcept -> expected<child> { return std::unexpected{waited.error()}; });
      co_return;
    }
    auto child = (*operation)->take_result();
    if (!result.try_commit(
            [&]() noexcept -> expected<::faio::process::child> { return std::move(child); })
        && child)
      (void)child->start_kill();
  };
  co_return co_await io::detail::run_operation<child>(options, std::move(body));
}
}  // namespace faio::process::detail
