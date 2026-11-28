/**
 * @file faio/detail/process/platform/windows/spawn.hpp
 * @brief 使用显式句柄继承白名单的 Windows 原生启动。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/process/platform/windows/native.hpp"

namespace faio::process::detail {
/** @brief 预留服务容量并打开三路标准流资源，任一步失败均由 RAII 回滚。 */
inline auto prepare_launch(command_snapshot snapshot) -> expected<prepared_launch> {
  auto service = process_service::instance();
  if (!service)
    return std::unexpected{service.error()};
  auto record = (*service)->reserve_record();
  if (!record)
    return std::unexpected{record.error()};
  prepared_launch result{std::move(snapshot), std::move(*record)};
  const auto& command = result.snapshot_.command();
  const stdio* slots[] = {&command.get_stdin(), &command.get_stdout(), &command.get_stderr()};
  for (int i{}; i < 3; ++i) {
    if (slots[i]->kind() == stdio_kind::PIPED) {
      auto pipe = native_pipe_pair::create(i == 0);
      if (!pipe)
        return std::unexpected{pipe.error()};
      result.pipes_[i] = std::move(*pipe);
      result.std_handles_[i] = reinterpret_cast<HANDLE>(result.pipes_[i]->child_native());
      continue;
    }
    if (slots[i]->kind() == stdio_kind::INHERIT) {
      const DWORD constants[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
      const auto original = ::GetStdHandle(constants[i]);
      if (original && original != INVALID_HANDLE_VALUE) {
        HANDLE copy{};
        if (::DuplicateHandle(::GetCurrentProcess(),
                              original,
                              ::GetCurrentProcess(),
                              &copy,
                              0,
                              TRUE,
                              DUPLICATE_SAME_ACCESS)) {
          result.child_handles_[i].reset(copy);
          result.std_handles_[i] = copy;
          continue;
        }
        const auto error = ::GetLastError();
        if (error != ERROR_INVALID_HANDLE)
          return std::unexpected{io::windows::make_windows_error(error)};
      }
    }
    auto path = std::filesystem::path{L"NUL"};
    DWORD access = i == 0 ? GENERIC_READ : GENERIC_WRITE;
    DWORD creation = OPEN_EXISTING;
    if (slots[i]->kind() == stdio_kind::FILE) {
      path = slots[i]->file_path();
      if (path.is_relative())
        path = result.snapshot_.cwd() / path;
      if (i != 0) {
        creation = *slots[i]->file_mode() == stdio_file_mode::APPEND ? OPEN_ALWAYS : CREATE_ALWAYS;
        if (*slots[i]->file_mode() == stdio_file_mode::APPEND)
          access = FILE_APPEND_DATA;
      }
    }
    SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
    HANDLE file = ::CreateFileW(path.c_str(),
                                access,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &security,
                                creation,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    result.child_handles_[i].reset(file);
    if (slots[i]->kind() == stdio_kind::FILE) {
      BY_HANDLE_FILE_INFORMATION info{};
      if (::GetFileType(file) != FILE_TYPE_DISK || !::GetFileInformationByHandle(file, &info)
          || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return std::unexpected{process_error(errc::UNSUPPORTED_STDIO)};
    }
    result.std_handles_[i] = file;
  }
  return result;
}

/** @brief 用固定快照执行原生创建，并将进程标识写入预留记录。 */
inline auto launch_native(prepared_launch& prepared) -> expected<native_process> {
  std::vector<wchar_t> environment;
  for (const auto& item : prepared.snapshot_.environment_entries()) {
    environment.insert(environment.end(), item.begin(), item.end());
    environment.push_back(L'\0');
  }
  environment.push_back(L'\0');
  if (environment.size() == 1)
    environment.push_back(L'\0');
  std::vector<std::wstring> arguments;
  for (const auto& arg : prepared.snapshot_.command().get_args()) {
    auto wide = widen(arg);
    if (!wide)
      return std::unexpected{wide.error()};
    arguments.push_back(std::move(*wide));
  }
  std::vector<HANDLE> handles;
  for (auto handle : prepared.std_handles_)
    if (std::find(handles.begin(), handles.end(), handle) == handles.end())
      handles.push_back(handle);
  SIZE_T bytes{};
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<std::byte> storage(bytes);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};

  struct attribute_owner {
    LPPROC_THREAD_ATTRIBUTE_LIST list; /**< 已初始化的属性表，内存由外层 storage 保持有效。 */

    /** @brief 销毁已初始化的扩展进程属性表。 */
    ~attribute_owner() { ::DeleteProcThreadAttributeList(list); }
  } owner{attributes};

  if (!::UpdateProcThreadAttribute(attributes,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles.data(),
                                   handles.size() * sizeof(HANDLE),
                                   nullptr,
                                   nullptr))
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof startup;
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = prepared.std_handles_[0];
  startup.StartupInfo.hStdOutput = prepared.std_handles_[1];
  startup.StartupInfo.hStdError = prepared.std_handles_[2];
  startup.lpAttributeList = attributes;
  DWORD remembered = ERROR_FILE_NOT_FOUND;
  PROCESS_INFORMATION process{};
  for (const auto& candidate : prepared.snapshot_.executable_candidates()) {
    std::wstring line = quote_argument(candidate.native());
    for (const auto& arg : arguments) {
      line.push_back(L' ');
      line += quote_argument(arg);
    }
    if (line.size() >= 32767)
      return std::unexpected{process_error(errc::INVALID_CONFIGURATION)};
    std::vector<wchar_t> mutable_line(line.begin(), line.end());
    mutable_line.push_back(L'\0');
    if (::CreateProcessW(candidate.c_str(),
                         mutable_line.data(),
                         nullptr,
                         nullptr,
                         TRUE,
                         prepared.snapshot_.command().get_creation_flags()
                             | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                         environment.data(),
                         prepared.snapshot_.cwd().c_str(),
                         &startup.StartupInfo,
                         &process)) {
      native_handle_type thread{process.hThread};
      {
        std::lock_guard lock(prepared.record_->mutex_);
        prepared.record_->pid_ = process.dwProcessId;
        prepared.record_->process_ = process.hProcess;
        prepared.record_->launching_ = false;
      }
      prepared.close_child_ends();
      return native_process{prepared.record_, process.dwProcessId};
    }
    const auto error = ::GetLastError();
    if (error == ERROR_ACCESS_DENIED)
      remembered = error;
    else if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
      return std::unexpected{io::windows::make_windows_error(error)};
  }
  return std::unexpected{io::windows::make_windows_error(remembered)};
}
}  // namespace faio::process::detail
#endif
