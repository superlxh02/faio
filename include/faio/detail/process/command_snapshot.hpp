/**
 * @file faio/detail/process/command_snapshot.hpp
 * @brief 在调用线程固定工作目录、环境和可执行文件搜索顺序。
 */
#pragma once
#include "faio/detail/process/command.hpp"
#if defined(_WIN32)
#include "faio/detail/process/platform/windows/encoding.hpp"
#endif
#include <cerrno>
#include <cstdlib>
#include <map>
#include <system_error>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#if !defined(_WIN32)
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif
namespace faio::process::detail {
/** @brief 验证 UTF-8 标量编码并拒绝嵌入 NUL，避免原生字符串被提前截断。 */
inline bool valid_utf8(std::string_view bytes) noexcept {
  std::uint32_t code{}, minimum{};
  unsigned left{};
  for (const unsigned char c : bytes) {
    if (!left) {
      if (c < 0x80) {
        if (!c)
          return false;
        continue;
      }
      if (c >= 0xc2 && c <= 0xdf) {
        left = 1;
        code = c & 31;
        minimum = 0x80;
      } else if (c >= 0xe0 && c <= 0xef) {
        left = 2;
        code = c & 15;
        minimum = 0x800;
      } else if (c >= 0xf0 && c <= 0xf4) {
        left = 3;
        code = c & 7;
        minimum = 0x10000;
      } else
        return false;
    } else {
      if ((c & 0xc0) != 0x80)
        return false;
      code = code * 64 + (c & 63);
      if (!--left && (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)))
        return false;
    }
  }
  return left == 0;
}

/** @brief 按当前平台环境变量的键相等规则比较两个 UTF-8 键。 */
inline bool env_key_equal(std::string_view left, std::string_view right) {
#if defined(_WIN32)
  auto a = widen(left), b = widen(right);
  return a && b ? native_key_equal(*a, *b) : left == right;
#else
  return left == right;
#endif
}

/** @brief 固定启动时可见的配置、目录、环境和所属 IO 上下文。 */
class command_snapshot {
 public:
  /** @brief 禁止复制调用时固定的启动输入，避免重复持有所有权或复制运行状态。 */
  command_snapshot(const command_snapshot&) = delete;
  /** @brief 调用时固定的启动输入不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const command_snapshot&) -> command_snapshot& = delete;
  /** @brief 转交已经固定的命令、目录、环境、候选路径及 IO 上下文。 */
  command_snapshot(command_snapshot&&) noexcept = default;
  /** @brief 替换本对象的命令、目录、环境、候选路径及 IO 上下文。 */
  auto operator=(command_snapshot&&) noexcept -> command_snapshot& = default;
  /** @brief 释放固定启动输入，不改变已经交付的子进程。 */
  ~command_snapshot() = default;

  /** @brief 借用调用时复制并固定的命令配置。 */
  auto command() const noexcept -> const ::faio::process::command& { return command_; }

  /** @brief 借用已固定的子进程绝对工作目录。 */
  auto cwd() const noexcept -> const std::filesystem::path& { return cwd_; }

  /** @brief 借用按子进程有效 PATH 顺序解析的绝对候选路径。 */
  auto executable_candidates() const noexcept -> std::span<const std::filesystem::path> {
    return candidates_;
  }

  /** @brief 借用已合并的原生环境条目，不重新读取父进程环境。 */
  auto environment_entries() const noexcept -> std::span<const std::filesystem::path::string_type> {
    return environment_;
  }

  /** @brief 借用捕获时所属的 IO 上下文。 */
  auto context() const noexcept -> const io::io_context& { return context_; }
#if !defined(_WIN32)
  /** @brief 借用调用线程在快照时的 Unix 信号屏蔽字。 */
  const sigset_t& signal_mask() const noexcept { return signal_mask_; }
#endif
 private:
  /** @brief 允许快照工厂填写校验后固定的目录、环境与候选路径。 */
  friend auto snapshot_command(const ::faio::process::command&, io::io_context)
      -> expected<command_snapshot>;

  /** @brief 复制命令并固定 IO 上下文，其余启动输入随后在调用线程填写。 */
  command_snapshot(::faio::process::command command, io::io_context context)
      : command_(std::move(command)), context_(std::move(context)) {}

  ::faio::process::command command_; /**< 调用时复制的完整命令，不再观察原 Command 的后续修改。 */
  std::filesystem::path cwd_;        /**< 已解析并规范化的子进程绝对工作目录。 */
  std::vector<std::filesystem::path> candidates_; /**< 按有效 PATH 顺序固定的绝对可执行候选路径。 */
  std::vector<std::filesystem::path::string_type>
      environment_;        /**< 按平台原生编码保存的最终环境条目。 */
  io::io_context context_; /**< 原生管道接入的 IO 上下文，启动交付前使用。 */
#if !defined(_WIN32)
  sigset_t signal_mask_{};
#endif
};

/** @brief 同步验证配置并固定目录、环境、PATH 和平台启动条件。 */
inline auto snapshot_command(const command& command, io::io_context context)
    -> expected<command_snapshot> {
  try {
    if (!context)
      return std::unexpected{process_error(errc::RUNTIME_UNAVAILABLE)};
    if (context.stopped())
      return std::unexpected{process_error(errc::RUNTIME_SHUTDOWN)};
    const auto invalid = [](std::string message = "invalid process configuration") {
      auto context = std::make_shared<error_context>();
      context->message = std::move(message);
      return std::unexpected{
          process_error(errc::INVALID_CONFIGURATION).with_context(std::move(context))};
    };
    const auto path_ok = [](const std::filesystem::path& path) {
      return !path.empty()
             && path.native().find(typename std::filesystem::path::value_type{})
                    == std::filesystem::path::string_type::npos;
    };
    if (!path_ok(command.get_program()))
      return invalid("program: empty path or embedded NUL");
    for (std::size_t i{}; i < command.get_args().size(); ++i)
      if (!valid_utf8(command.get_args()[i]))
        return invalid("argv[" + std::to_string(i) + "]: invalid UTF-8 or embedded NUL");
    for (std::size_t i{}; i < command.get_envs().size(); ++i) {
      const auto& item = command.get_envs()[i];
      if (item.key.empty() || item.key.find('=') != std::string::npos || !valid_utf8(item.key)
          || (item.value && !valid_utf8(*item.value)))
        return invalid("environment[" + std::to_string(i) + "]: invalid key or UTF-8/NUL");
    }
    const stdio* slots[] = {&command.get_stdin(), &command.get_stdout(), &command.get_stderr()};
    for (int i{}; i < 3; ++i)
      if (slots[i]->kind() == stdio_kind::FILE) {
        if (!path_ok(slots[i]->file_path()) || !slots[i]->file_mode()
            || ((i == 0) != (*slots[i]->file_mode() == stdio_file_mode::READ)))
          return invalid("stdio[" + std::to_string(i) + "]: invalid file binding");
      }
#if defined(_WIN32)
    constexpr DWORD priority = IDLE_PRIORITY_CLASS | BELOW_NORMAL_PRIORITY_CLASS
                               | NORMAL_PRIORITY_CLASS | ABOVE_NORMAL_PRIORITY_CLASS
                               | HIGH_PRIORITY_CLASS;
    constexpr DWORD allowed = CREATE_NO_WINDOW | CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP
                              | DETACHED_PROCESS | CREATE_DEFAULT_ERROR_MODE | priority
                              | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    const DWORD flags = command.get_creation_flags(), selected = flags & priority;
    if (flags & ~allowed)
      return std::unexpected{process_error(errc::UNSUPPORTED_CREATION_FLAGS)};
    if ((selected && (selected & (selected - 1)))
        || ((flags & CREATE_NEW_CONSOLE) && (flags & (CREATE_NO_WINDOW | DETACHED_PROCESS))))
      return invalid("creation_flags: conflicting priority or console flags");
#else
    if (command.get_process_group() && *command.get_process_group() < 0)
      return invalid("process_group: negative group");
    if (command.get_arg0() && !valid_utf8(*command.get_arg0()))
      return invalid("arg0: invalid UTF-8 or embedded NUL");
    struct sigaction action{};
    if (::sigaction(SIGCHLD, nullptr, &action) < 0)
      return std::unexpected{make_error(errno)};
    if (action.sa_handler == SIG_IGN || (action.sa_flags & SA_NOCLDWAIT))
      return invalid("SIGCHLD: SIG_IGN/SA_NOCLDWAIT prevents exclusive reaping");
#endif
    command_snapshot result{command, std::move(context)};
    std::error_code ec;
    result.cwd_ = std::filesystem::current_path(ec);
    if (ec)
      return std::unexpected{make_error(ec.value())};
    if (command.get_current_dir()) {
      if (!path_ok(*command.get_current_dir()))
        return invalid("current_dir: empty path or embedded NUL");
      result.cwd_ =
          (command.get_current_dir()->is_absolute() ? *command.get_current_dir()
                                                    : result.cwd_ / *command.get_current_dir())
              .lexically_normal();
    }
    using native_string = std::filesystem::path::string_type;
#if defined(_WIN32)
    struct less {
      /** @brief 按 Windows 忽略大小写的序数规则比较两个环境键的顺序。 */
      bool operator()(const std::wstring& a, const std::wstring& b) const {
        return native_key_less(a, b);
      }
    };

    std::map<native_string, native_string, less> environment;
    if (!command.get_env_clear()) {
      auto* block = ::GetEnvironmentStringsW();
      if (!block)
        return std::unexpected{io::windows::make_windows_error(::GetLastError())};
      try {
        for (auto* item = block; *item; item += std::wcslen(item) + 1) {
          std::wstring entry{item};
          const auto pos = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
          if (pos != std::wstring::npos)
            environment[entry.substr(0, pos)] = entry.substr(pos + 1);
        }
      } catch (...) {
        ::FreeEnvironmentStringsW(block);
        throw;
      }
      ::FreeEnvironmentStringsW(block);
    }
    for (const auto& change : command.get_envs()) {
      auto key = widen(change.key);
      if (!key)
        return invalid();
      environment.erase(*key);
      if (change.value) {
        auto value = widen(*change.value);
        if (!value)
          return invalid();
        environment.emplace(std::move(*key), std::move(*value));
      }
    }
    std::wstring path;
    if (auto it = environment.find(L"PATH"); it != environment.end())
      path = it->second;
    else {
      wchar_t buffer[32768];
      const UINT n = ::GetSystemDirectoryW(buffer, 32768);
      if (n && n < 32768)
        path.assign(buffer, n);
      const UINT m = ::GetWindowsDirectoryW(buffer, 32768);
      if (m && m < 32768) {
        if (!path.empty())
          path.push_back(L';');
        path.append(buffer, m);
      }
    }
    constexpr wchar_t separator = L';';
#else
    std::map<native_string, native_string> environment;
    if (!command.get_env_clear()) {
#if defined(__APPLE__)
      auto** source = *_NSGetEnviron();
#else
      auto** source = ::environ;
#endif
      for (; source && *source; ++source) {
        std::string entry{*source};
        const auto pos = entry.find('=');
        if (pos != std::string::npos)
          environment[entry.substr(0, pos)] = entry.substr(pos + 1);
      }
    }
    for (const auto& change : command.get_envs()) {
      if (change.value)
        environment[change.key] = *change.value;
      else
        environment.erase(change.key);
    }
    std::string path;
    if (auto it = environment.find("PATH"); it != environment.end())
      path = it->second;
    else {
      const auto size = ::confstr(_CS_PATH, nullptr, 0);
      if (size) {
        path.resize(size);
        ::confstr(_CS_PATH, path.data(), size);
        path.pop_back();
      }
    }
    constexpr char separator = ':';
    const int mask_error = ::pthread_sigmask(SIG_SETMASK, nullptr, &result.signal_mask_);
    if (mask_error)
      return std::unexpected{make_error(mask_error)};
#endif
    for (const auto& [key, value] : environment)
      result.environment_.push_back(key + typename native_string::value_type{'='} + value);
    auto program = command.get_program();
#if defined(_WIN32)
    if ((program.has_root_name() && !program.has_root_directory())
        || (!program.has_root_name() && program.has_root_directory()))
      return invalid("program: drive-relative path or rooted path without drive");
    if (!program.has_extension())
      program += L".exe";
#endif
    if (program.is_absolute())
      result.candidates_.push_back(program);
    else if (program.has_parent_path())
      result.candidates_.push_back((result.cwd_ / program).lexically_normal());
    else {
      std::size_t begin{};
      for (;;) {
        const auto end = path.find(separator, begin);
        const auto item = path.substr(begin, end == native_string::npos ? end : end - begin);
        auto directory = item.empty() ? result.cwd_ : std::filesystem::path{item};
        if (directory.is_relative())
          directory = result.cwd_ / directory;
        result.candidates_.push_back((directory / program).lexically_normal());
        if (end == native_string::npos)
          break;
        begin = end + 1;
      }
    }
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected{make_error(ENOMEM)};
  } catch (const std::filesystem::filesystem_error& error) {
    return std::unexpected{make_error(error.code().value())};
  }
}
}  // namespace faio::process::detail

namespace faio::process {
/** @brief 在原位置替换相同环境键，避免同一键出现多项修改。 */
inline void command::set_env(std::string key, std::optional<std::string> value) {
  for (auto& item : envs_)
    if (detail::env_key_equal(item.key, key)) {
      item = {std::move(key), std::move(value)};
      return;
    }
  envs_.push_back({std::move(key), std::move(value)});
}
}  // namespace faio::process
