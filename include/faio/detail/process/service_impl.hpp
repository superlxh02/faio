/**
 * @file faio/detail/process/service_impl.hpp
 * @brief 原生进程退出查询、监视注册及服务回收实现。
 */
#pragma once
#include "faio/detail/process/exit_wait_operation.hpp"
#include "faio/detail/process/launch_operation.hpp"
#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#endif
namespace faio::process::detail {
/** @brief 撤销运行时关闭回调并释放仍由记录持有的平台监视资源。 */
inline ProcessState::~ProcessState() {
  if (auto domain = runtime_.lock(); domain && shutdown_token_)
    io::io_context{domain}.unregister_shutdown_cleanup(shutdown_token_);
#if defined(_WIN32)
  if (process_)
    ::CloseHandle(process_);
#else
  if (watch_fd_ >= 0)
    ::close(watch_fd_);
#endif
}

/** @brief 通过弱引用登记运行时关闭回调，空闲子进程不会延长运行时的生命周期。 */
inline void ProcessState::attach_runtime(const io::io_context& context) {
  runtime_ = context.domain();
  shutdown_token_ = context.register_shutdown_cleanup([weak = weak_from_this()] {
    if (auto state = weak.lock())
      state->release_runtime_attachment();
  });
}

/** @brief 在退出记录上安装一个弱等待者；已终止时无需挂起。 */
inline auto ProcessState::attach_waiter(const std::shared_ptr<ExitWaitOperation>& operation)
    -> expected<bool> {
  std::lock_guard lock(mutex_);
  if (runtime_stopped())
    return std::unexpected{process_error(errc::runtime_shutdown)};
  if (terminal_)
    return false;
  if (!waiter_.expired())
    return std::unexpected{process_error(errc::operation_in_progress)};
  waiter_ = operation;
  return true;
}

/** @brief 仅移除仍与本次操作相同的等待者，避免误删后续等待。 */
inline void ProcessState::detach_waiter(
    const std::shared_ptr<ExitWaitOperation>& operation) noexcept {
  std::lock_guard lock(mutex_);
  if (waiter_.lock() == operation)
    waiter_.reset();
}

/** @brief 从等待者的弱引用取得强引用后，在记录锁外发布完成。 */
inline void ProcessState::notify_waiter() {
  std::shared_ptr<ExitWaitOperation> waiter;
  {
    std::lock_guard lock(mutex_);
    waiter = waiter_.lock();
  }
  if (waiter)
    waiter->notify_terminal();
}

/** @brief 在结果锁内发布首个终态，再在锁外唤醒等待者。 */
inline auto ProcessState::commit_exit(expected<ExitStatus> value) -> bool {
  {
    std::lock_guard lock(mutex_);
    if (terminal_)
      return false;
    terminal_ = true;
    if (value)
      status_ = *value;
    else
      error_ = value.error();
  }
  notify_waiter();
  return true;
}

/** @brief 停止本运行时上的等待，不阻止独立服务继续回收子进程。 */
inline void ProcessState::release_runtime_attachment() noexcept {
  runtime_stopped_.store(true);
  std::shared_ptr<ExitWaitOperation> waiter;
  {
    std::lock_guard lock(mutex_);
    waiter = waiter_.lock();
  }
  if (waiter)
    waiter->request_stop(process_error(errc::runtime_shutdown));
}

/** @brief 持有记录锁时，以非阻塞方式查询该记录对应的进程，并缓存首个终态。 */
inline auto ProcessState::poll_locked() -> expected<std::optional<ExitStatus>> {
  if (error_)
    return std::unexpected{*error_};
  if (status_)
    return status_;
  if (!pid_)
    return std::optional<ExitStatus>{};
#if defined(_WIN32)
  const DWORD ready = ::WaitForSingleObject(process_, 0);
  if (ready == WAIT_TIMEOUT)
    return std::optional<ExitStatus>{};
  if (ready != WAIT_OBJECT_0) {
    error_ = io::windows::make_windows_error(::GetLastError());
    terminal_ = true;
    return std::unexpected{*error_};
  }
  DWORD code{};
  if (!::GetExitCodeProcess(process_, &code)) {
    error_ = io::windows::make_windows_error(::GetLastError());
    terminal_ = true;
    return std::unexpected{*error_};
  }
  status_ = ExitStatus::from_raw_windows(code);
  terminal_ = true;
#else
  int status{};
  pid_t result;
  do {
    result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  } while (result < 0 && errno == EINTR);
  if (result == 0)
    return std::optional<ExitStatus>{};
  if (result < 0) {
    error_ = errno == ECHILD ? process_error(errc::child_not_waitable) : make_error(errno);
    terminal_ = true;
    return std::unexpected{*error_};
  }
  auto parsed = ExitStatus::from_raw_unix(status);
  if (!parsed) {
    error_ = parsed.error();
    terminal_ = true;
    return std::unexpected{*error_};
  }
  status_ = *parsed;
  terminal_ = true;
#endif
  return status_;
}

/** @brief 查询记录对应进程的缓存状态，发现终态后通知等待者和回收服务。 */
inline auto poll_exit(const std::shared_ptr<ProcessState>& state)
    -> expected<std::optional<ExitStatus>> {
  if (!state)
    return std::unexpected{make_error(EBADF)};
  auto result = [&] {
    std::lock_guard lock(state->mutex_);
    return state->poll_locked();
  }();
  if (!result || *result) {
    state->notify_waiter();
    if (auto service = ProcessService::instance())
      (*service)->notify_control_change();
  }
  return result;
}

/** @brief 持有记录锁以避免 PID 复用窗口，只对该记录对应且尚未退出的进程请求终止。 */
inline auto request_termination(const std::shared_ptr<ProcessState>& state) -> expected<void> {
  if (!state)
    return std::unexpected{make_error(EBADF)};
  expected<void> result;
  {
    std::lock_guard lock(state->mutex_);
    const auto exited = state->poll_locked();
    if (!exited)
      return std::unexpected{exited.error()};
    if (*exited)
      return {};
    if (!state->pid_)
      return std::unexpected{make_error(EBADF)};
#if defined(_WIN32)
    if (!::TerminateProcess(state->process_, 1))
      result = std::unexpected{io::windows::make_windows_error(::GetLastError())};
#else
    if (::kill(static_cast<pid_t>(state->pid_), SIGKILL) < 0 && errno != ESRCH)
      result = std::unexpected{make_error(errno)};
#endif
    state->kill_requested_ = true;
  }
  if (auto service = ProcessService::instance())
    (*service)->notify_control_change();
  return result;
}

/** @brief 建立平台控制通知源并启动固定数量的服务线程。 */
inline ProcessService::ProcessService() {
#if defined(__linux__)
  poller_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (poller_ < 0)
    throw std::system_error(errno, std::generic_category());
  control_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (control_ < 0) {
    int e = errno;
    ::close(poller_);
    throw std::system_error(e, std::generic_category());
  }
  epoll_event event{};
  event.events = EPOLLIN;
  event.data.u64 = 0;
  if (::epoll_ctl(poller_, EPOLL_CTL_ADD, control_, &event) < 0) {
    int e = errno;
    ::close(control_);
    ::close(poller_);
    throw std::system_error(e, std::generic_category());
  }
#elif defined(__APPLE__)
  poller_ = ::kqueue();
  if (poller_ < 0)
    throw std::system_error(errno, std::generic_category());
  ::fcntl(poller_, F_SETFD, FD_CLOEXEC);
  struct kevent event;
  EV_SET(&event, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  if (::kevent(poller_, &event, 1, nullptr, 0, nullptr) < 0) {
    int e = errno;
    ::close(poller_);
    throw std::system_error(e, std::generic_category());
  }
#endif
  try {
#if !defined(_WIN32)
    monitor_ = std::thread([this] { monitor_loop(); });
#endif
    launch_ = std::thread([this] { launch_loop(); });
  } catch (...) {
    stopping_.store(true);
    notify_control_change();
#if !defined(_WIN32)
    if (monitor_.joinable())
      monitor_.join();
#endif
#if !defined(_WIN32)
    ::close(poller_);
#if defined(__linux__)
    ::close(control_);
#endif
#endif
    throw;
  }
}

/** @brief 按需取得程序共享服务，资源创建失败时转换为 expected 错误。 */
inline auto ProcessService::instance() -> expected<ProcessService*> {
  try {
    static ProcessService service;
    return &service;
  } catch (const std::bad_alloc&) {
    return std::unexpected{make_error(ENOMEM)};
  } catch (const std::system_error& error) {
    return std::unexpected{make_error(error.code().value())};
  }
}

/** @brief 从固定容量记录表预留空槽，并分配新的代际令牌。 */
inline auto ProcessService::reserve_record() -> expected<std::shared_ptr<ProcessState>> {
  try {
    std::lock_guard lock(mutex_);
    if (stopping_)
      return std::unexpected{process_error(errc::runtime_shutdown)};
    for (auto& slot : records_)
      if (!slot) {
        struct factory final : ProcessState {
          /** @brief 原样转发构造参数，通过 make_shared 在一次分配中创建地址固定的状态。 */
          explicit factory(std::uint64_t token) : ProcessState(token) {}
        };

        slot = std::make_shared<factory>(++next_token_);
        slot->launching_ = true;
        return slot;
      }
    return std::unexpected{process_error(errc::process_limit_reached)};
  } catch (const std::bad_alloc&) {
    return std::unexpected{make_error(ENOMEM)};
  }
}

/** @brief 移除指定记录所占槽位，释放预留失败或已回收的服务容量。 */
inline void ProcessService::abandon_reserved(const std::shared_ptr<ProcessState>& state) noexcept {
  if (!state)
    return;
  std::lock_guard lock(mutex_);
  for (auto& slot : records_)
    if (slot == state) {
      slot.reset();
      return;
    }
}

/** @brief 归还尚未使用的预留记录，未交付的原生资源由成员通过 RAII 释放。 */
inline PreparedLaunch::~PreparedLaunch() {
  release_record();
}

/** @brief 只归还尚未创建原生进程的预留记录，已启动记录留给服务回收。 */
inline void PreparedLaunch::release_record() noexcept {
  if (record_) {
    bool launched;
    {
      std::lock_guard lock(record_->mutex_);
      launched = record_->pid_ != 0;
    }
    if (!launched)
      if (auto service = ProcessService::instance())
        (*service)->abandon_reserved(record_);
  }
}

/** @brief 对未完成服务交接的进程请求终止，并恢复服务的回收路径。 */
inline void NativeProcess::cleanup() noexcept {
  if (!pid_ || !record_)
    return;
  (void)request_termination(record_);
  if (auto service = ProcessService::instance()) {
    std::lock_guard lock(record_->mutex_);
    record_->watch_ready_ = true;
#if !defined(_WIN32)
    record_->fallback_ = true;
#endif
    (*service)->notify_control_change();
  }
  pid_ = 0;
  record_.reset();
}

/** @brief 注册平台退出通知并接管原生进程的后续回收责任。 */
inline auto ProcessService::watch(const std::shared_ptr<ProcessState>& state, NativeProcess native)
    -> expected<void> {
  expected<void> watch_result;
  (void)poll_exit(state);
  {
    std::lock_guard lock(state->mutex_);
#if defined(__linux__)
    if (!state->terminal_) {
#ifdef SYS_pidfd_open
      const int fd =
          static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(state->pid_), 0));
#else
      const int fd = -1;
#endif
      if (fd >= 0) {
        epoll_event event{};
        event.events = EPOLLIN | EPOLLONESHOT;
        event.data.u64 = state->token_;
        if (::epoll_ctl(poller_, EPOLL_CTL_ADD, fd, &event) == 0)
          state->watch_fd_ = fd;
        else
          ::close(fd);
      }
      state->fallback_ = state->watch_fd_ < 0;
    }
#elif defined(__APPLE__)
    if (!state->terminal_) {
      struct kevent event;
      EV_SET(&event,
             state->pid_,
             EVFILT_PROC,
             EV_ADD | EV_ONESHOT,
             NOTE_EXIT,
             0,
             reinterpret_cast<void*>(static_cast<std::uintptr_t>(state->token_)));
      state->fallback_ = ::kevent(poller_, &event, 1, nullptr, 0, nullptr) < 0;
    }
#else
    if (!state->terminal_) {
      state->native_wait_ = ::CreateThreadpoolWait(&native_callback, state.get(), nullptr);
      if (state->native_wait_)
        ::SetThreadpoolWait(state->native_wait_, state->process_, nullptr);
      else {
        watch_result = std::unexpected{io::windows::make_windows_error(::GetLastError())};
        if (state->process_)
          ::TerminateProcess(state->process_, 1);
      }
    }
#endif
    state->watch_ready_ = true;
  }
  native.pid_ = 0;
  native.record_.reset();
  (void)poll_exit(state);
  notify_control_change();
  return watch_result;
}

/** @brief 增加控制版本并唤醒服务线程重新计算工作与等待期限。 */
inline void ProcessService::notify_control_change() noexcept {
  revision_.fetch_add(1);
  cv_.notify_one();
#if defined(__linux__)
  const std::uint64_t one = 1;
  (void)::write(control_, &one, sizeof one);
#elif defined(__APPLE__)
  struct kevent event;
  EV_SET(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
  (void)::kevent(poller_, &event, 1, nullptr, 0, nullptr);
#endif
}

/** @brief 根据等待截止时间及必要的轮询回退，选择最近的唤醒时间。 */
inline auto ProcessService::next_deadline()
    -> std::optional<std::chrono::steady_clock::time_point> {
  std::array<std::shared_ptr<ProcessState>, 4096> snapshot;
  {
    std::lock_guard lock(mutex_);
    snapshot = records_;
  }
  std::optional<std::chrono::steady_clock::time_point> result;
  const auto consider = [&](auto value) {
    if (!result || value < *result)
      result = value;
  };
  for (const auto& state : snapshot)
    if (state) {
      std::shared_ptr<ExitWaitOperation> waiter;
      {
        std::lock_guard lock(state->mutex_);
        if (!state->watch_ready_)
          continue;
#if defined(_WIN32)
        if (state->terminal_ || !state->native_wait_)
          consider(std::chrono::steady_clock::now() + std::chrono::milliseconds(10));
#else
        if (state->fallback_ && !state->terminal_)
          consider(std::chrono::steady_clock::now() + std::chrono::milliseconds(10));
#endif
        waiter = state->waiter_.lock();
      }
      if (waiter && waiter->deadline())
        consider(*waiter->deadline());
    }
  return result;
}

/** @brief 查询已注册记录，结束已到期的等待，并在终态后释放平台监视资源。 */
inline void ProcessService::sweep() noexcept {
  std::array<std::shared_ptr<ProcessState>, 4096> snapshot;
  {
    std::lock_guard lock(mutex_);
    snapshot = records_;
  }
  for (const auto& state : snapshot)
    if (state) {
      bool ready;
      {
        std::lock_guard lock(state->mutex_);
        ready = state->watch_ready_;
      }
      if (!ready)
        continue;
      (void)poll_exit(state);
      std::shared_ptr<ExitWaitOperation> waiter;
      bool terminal;
      {
        std::lock_guard lock(state->mutex_);
        terminal = state->terminal_;
        waiter = state->waiter_.lock();
      }
      if (waiter && waiter->deadline() && std::chrono::steady_clock::now() >= *waiter->deadline())
        waiter->request_stop(make_error(ETIMEDOUT));
      if (!terminal)
        continue;
#if defined(_WIN32)
      PTP_WAIT native_wait{};
      {
        std::lock_guard lock(state->mutex_);
        native_wait = std::exchange(state->native_wait_, nullptr);
      }
      if (native_wait) {
        ::SetThreadpoolWait(native_wait, nullptr, nullptr);
        ::WaitForThreadpoolWaitCallbacks(native_wait, TRUE);
        ::CloseThreadpoolWait(native_wait);
      }
#else
      {
        std::lock_guard lock(state->mutex_);
        if (state->watch_fd_ >= 0) {
#if defined(__linux__)
          (void)::epoll_ctl(poller_, EPOLL_CTL_DEL, state->watch_fd_, nullptr);
#endif
          ::close(std::exchange(state->watch_fd_, -1));
        }
      }
#endif
      abandon_reserved(state);
    }
}
#if !defined(_WIN32)
/** @brief 由 Unix 固定监视线程等待退出通知、超时和控制变更。 */
inline void ProcessService::monitor_loop() noexcept {
  while (!stopping_) {
    const auto deadline = next_deadline();
    int timeout = -1;
    if (deadline)
      timeout = static_cast<int>(std::clamp<std::int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(*deadline
                                                                - std::chrono::steady_clock::now())
                  .count()
              + 1,
          0,
          INT_MAX));
#if defined(__linux__)
    epoll_event events[64];
    const int count = ::epoll_wait(poller_, events, 64, timeout);
    if (count < 0 && errno != EINTR)
      break;
    std::uint64_t value{};
    while (::read(control_, &value, sizeof value) > 0) {
    }
#else
    struct kevent events[64];
    timespec interval{timeout / 1000, (timeout % 1000) * 1000000};
    const int count = ::kevent(poller_, nullptr, 0, events, 64, timeout < 0 ? nullptr : &interval);
    if (count < 0 && errno != EINTR)
      break;
#endif
    sweep();
  }
  sweep();
}
#else
/** @brief 在 Windows 等待回调中缓存退出并唤醒服务，回调自身不做排空等待。 */
inline void CALLBACK ProcessService::native_callback(PTP_CALLBACK_INSTANCE,
                                                     void* context,
                                                     PTP_WAIT,
                                                     TP_WAIT_RESULT) noexcept {
  auto* state = static_cast<ProcessState*>(context);
  {
    std::lock_guard lock(state->mutex_);
    (void)state->poll_locked();
  }
  state->notify_waiter();
  if (auto service = instance())
    (*service)->notify_control_change();
}
#endif
/** @brief 记录公共控制器已释放，并按策略请求终止子进程。 */
inline void ProcessService::release_public_control(const std::shared_ptr<ProcessState>& state,
                                                   bool kill) noexcept {
  if (kill)
    (void)request_termination(state);
  {
    std::lock_guard lock(state->mutex_);
    state->public_control_released_ = true;
  }
}

/** @brief 将公共 Child 的析构策略交给共享回收服务。 */
inline void release_control(const std::shared_ptr<ProcessState>& state, bool kill) noexcept {
  if (auto service = ProcessService::instance())
    (*service)->release_public_control(state, kill);
}

/** @brief 取消指定 IO 域上的活动等待，保留原生进程回收责任。 */
inline void ProcessService::detach_runtime(const io::io_context& context) noexcept {
  std::array<std::shared_ptr<ProcessState>, 4096> snapshot;
  {
    std::lock_guard lock(mutex_);
    snapshot = records_;
  }
  for (auto& state : snapshot)
    if (state && state->runtime_.lock() == context.domain())
      state->release_runtime_attachment();
}
}  // namespace faio::process::detail
