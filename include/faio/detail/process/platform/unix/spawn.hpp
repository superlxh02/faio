/**
 * @file faio/detail/process/platform/unix/spawn.hpp
 * @brief 使用 posix_spawn 的 Unix 标准流准备和原生启动。
 */
#pragma once
#if !defined(_WIN32)
#include "faio/detail/process/platform/unix/native.hpp"
#include <spawn.h>
#include <sys/stat.h>

namespace faio::process::detail {
/** @brief 预留服务容量并打开三路标准流资源，任一步失败均由 RAII 回滚。 */
inline auto prepare_launch(CommandSnapshot snapshot) -> expected<PreparedLaunch> {
  auto service = ProcessService::instance();
  if (!service)
    return std::unexpected{service.error()};
  auto record = (*service)->reserve_record();
  if (!record)
    return std::unexpected{record.error()};
  PreparedLaunch result{std::move(snapshot), std::move(*record)};
  const auto& command = result.snapshot_.command();
  const Stdio* slots[] = {&command.get_stdin(), &command.get_stdout(), &command.get_stderr()};
  std::lock_guard creation{spawn_creation_mutex()};
  for (int i{}; i < 3; ++i) {
    if (slots[i]->kind() == StdioKind::inherit) {
      if (::fcntl(i, F_GETFD) < 0) {
        if (errno != EBADF)
          return std::unexpected{make_error(errno)};
        continue;
      }
      auto copy = lift_fd(::fcntl(i, F_DUPFD_CLOEXEC, 3));
      if (!copy)
        return std::unexpected{copy.error()};
      result.files_[i] = std::move(*copy);
      result.child_fds_[i] = result.files_[i].get();
      continue;
    }
    if (slots[i]->kind() == StdioKind::piped) {
      auto pipe = NativePipePair::create(i == 0);
      if (!pipe)
        return std::unexpected{pipe.error()};
      result.pipes_[i] = std::move(*pipe);
      result.child_fds_[i] = static_cast<int>(result.pipes_[i]->child_native());
      continue;
    }
    int flags = i == 0 ? O_RDONLY : O_WRONLY;
    auto path = std::filesystem::path{"/dev/null"};
    if (slots[i]->kind() == StdioKind::file) {
      path = slots[i]->file_path();
      if (path.is_relative())
        path = result.snapshot_.cwd() / path;
      if (i != 0)
        flags |= O_CREAT | (*slots[i]->file_mode() == StdioFileMode::append ? O_APPEND : O_TRUNC);
    }
    auto opened = lift_fd(::open(path.c_str(), flags | O_CLOEXEC | O_NONBLOCK, 0666));
    if (!opened)
      return std::unexpected{opened.error()};
    if (slots[i]->kind() == StdioKind::file) {
      struct stat st{};
      if (::fstat(opened->get(), &st) < 0)
        return std::unexpected{make_error(errno)};
      if (!S_ISREG(st.st_mode))
        return std::unexpected{process_error(errc::unsupported_stdio)};
    }
    const int old = ::fcntl(opened->get(), F_GETFL);
    if (old < 0 || ::fcntl(opened->get(), F_SETFL, old & ~O_NONBLOCK) < 0)
      return std::unexpected{make_error(errno)};
    result.files_[i] = std::move(*opened);
    result.child_fds_[i] = result.files_[i].get();
  }
  return result;
}

/** @brief 用固定快照执行原生创建，并将进程标识写入预留记录。 */
inline auto launch_native(PreparedLaunch& prepared) -> expected<NativeProcess> {
  struct actions_owner {
    posix_spawn_file_actions_t value; /**< 按顺序记录子进程 chdir、dup2 与关闭动作。 */
    bool initialized{};               /**< 仅在初始化成功后，才在析构时调用对应的销毁函数。 */

    /** @brief 仅在初始化成功时销毁文件操作表。 */
    ~actions_owner() {
      if (initialized)
        ::posix_spawn_file_actions_destroy(&value);
    }
  } actions;

  struct attr_owner {
    posix_spawnattr_t value; /**< 记录子进程信号屏蔽字、默认信号和进程组属性。 */
    bool initialized{};      /**< 仅在初始化成功后，才在析构时调用对应的销毁函数。 */

    /** @brief 仅在初始化成功时销毁原生启动属性。 */
    ~attr_owner() {
      if (initialized)
        ::posix_spawnattr_destroy(&value);
    }
  } attributes;

  int error = ::posix_spawn_file_actions_init(&actions.value);
  if (error)
    return std::unexpected{make_error(error)};
  actions.initialized = true;
  error = ::posix_spawnattr_init(&attributes.value);
  if (error)
    return std::unexpected{make_error(error)};
  attributes.initialized = true;
  if ((error = ::posix_spawn_file_actions_addchdir_np(&actions.value,
                                                      prepared.snapshot_.cwd().c_str())))
    return std::unexpected{make_error(error)};
  for (int i{}; i < 3; ++i) {
    error = prepared.child_fds_[i] < 0
                ? ::posix_spawn_file_actions_addclose(&actions.value, i)
                : ::posix_spawn_file_actions_adddup2(&actions.value, prepared.child_fds_[i], i);
    if (error)
      return std::unexpected{make_error(error)};
  }
  short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__linux__)
  if ((error = ::posix_spawn_file_actions_addclosefrom_np(&actions.value, 3)))
    return std::unexpected{make_error(error)};
#else
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  const auto& command = prepared.snapshot_.command();
  if (command.get_process_group()) {
    flags |= POSIX_SPAWN_SETPGROUP;
    if ((error = ::posix_spawnattr_setpgroup(&attributes.value, *command.get_process_group())))
      return std::unexpected{make_error(error)};
  }
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  if ((error = ::posix_spawnattr_setsigdefault(&attributes.value, &defaults))
      || (error =
              ::posix_spawnattr_setsigmask(&attributes.value, &prepared.snapshot_.signal_mask()))
      || (error = ::posix_spawnattr_setflags(&attributes.value, flags)))
    return std::unexpected{make_error(error)};
  std::vector<std::string> arguments;
  arguments.reserve(command.get_args().size() + 1);
  arguments.push_back(command.get_arg0() ? *command.get_arg0() : command.get_program().native());
  for (const auto& arg : command.get_args())
    arguments.push_back(arg);
  std::vector<char*> argv, envp;
  for (auto& arg : arguments)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  for (const auto& item : prepared.snapshot_.environment_entries())
    envp.push_back(const_cast<char*>(item.c_str()));
  envp.push_back(nullptr);
  int denied{};
  pid_t pid{};
  {
    std::lock_guard creation{spawn_creation_mutex()};
    for (const auto& candidate : prepared.snapshot_.executable_candidates()) {
      error = ::posix_spawn(
          &pid, candidate.c_str(), &actions.value, &attributes.value, argv.data(), envp.data());
      if (!error)
        break;
      if (error == EACCES) {
        denied = error;
        continue;
      }
      if (error != ENOENT && error != ENOTDIR)
        return std::unexpected{make_error(error)};
    }
  }
  if (error)
    return std::unexpected{make_error(denied ? denied : error)};
  {
    std::lock_guard lock(prepared.record_->mutex_);
    prepared.record_->pid_ = static_cast<process_id>(pid);
    prepared.record_->launching_ = false;
  }
  prepared.close_child_ends();
  return NativeProcess{prepared.record_, static_cast<process_id>(pid)};
}
}  // namespace faio::process::detail
#endif
