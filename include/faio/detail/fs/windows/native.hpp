#pragma once
/** @file native.hpp @brief Windows 文件系统的 Unicode、句柄与属性适配。 */
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/platform/windows_handle.hpp"
#include "faio/detail/io/platform/windows_error.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>
#include <winternl.h>

namespace faio::fs::detail {
/** @brief 把路径固定为绝对 UTF-16 扩展路径，支持中文、UNC 和长路径。
 * @details 不经过 ANSI 代码页；已有设备路径保持原表示。参数由任务拥有，
 *          系统调用期间不会借用调用者的临时 string/path。
 */
inline expected<std::wstring> native_path(const std::filesystem::path& path) {
  const auto& name = path.native();  // Windows filesystem::path 原生字符是 wchar_t。
  if (name.empty() || name.find(L'\0') != std::wstring::npos)
    return std::unexpected{make_error(EINVAL)};  // 避免截断路径造成错误对象操作。
  if (name.starts_with(L"\\\\?\\") || name.starts_with(L"\\\\.\\"))
    return name;  // 保留调用者明确选择的设备/扩展路径语义。
  DWORD required = ::GetFullPathNameW(name.c_str(), 0, nullptr, nullptr);
  if (!required)
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  std::wstring result(required, L'\0');  // 大小包含 Windows 要求的终止符容量。
  for (;;) {
    const DWORD length =
        ::GetFullPathNameW(name.c_str(), static_cast<DWORD>(result.size()), result.data(), nullptr);
    if (!length)
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    if (length < result.size()) {
      result.resize(length);  // 拥有型字符串自身添加终止符。
      break;
    }
    result.resize(static_cast<std::size_t>(length) + 1);  // 容量不足时只重试转换。
  }
  std::replace(result.begin(), result.end(), L'/', L'\\');
  if (result.starts_with(L"\\\\"))
    return L"\\\\?\\UNC\\" + result.substr(2);  // UNC 扩展形式与盘符形式不同。
  return L"\\\\?\\" + result;
}

/** @brief 判断真实 HANDLE，禁止把 NULL 和 INVALID_HANDLE_VALUE 提交内核。 */
inline bool valid_handle(HANDLE handle) noexcept {
  return handle && handle != INVALID_HANDLE_VALUE;
}

/** @brief 使用已固定的父目录句柄打开一个直接子项，不跟随重解析点。
 * @param parent 已打开父目录；重命名父目录不能改变此解析锚点。
 * @param name 单个 UTF-16 文件名，禁止分隔符与父目录跳转。
 * @param access 调用方要求的权限，删除使用 DELETE，枚举使用 FILE_LIST_DIRECTORY。
 * @details NtCreateFile 的 RootDirectory 提供 Windows 公开 Win32 路径接口所缺少
 *          的句柄相对解析。只使用稳定 ntdll 导出；失败保持原生错误语义。
 */
inline expected<io::windows::owned_file_handle> open_relative(HANDLE parent,
                                                              std::wstring_view name,
                                                              ACCESS_MASK access) {
  if (name.empty() || name == L"." || name == L".."
      || name.find_first_of(L"\\/:") != std::wstring_view::npos
      || name.size() > (std::numeric_limits<USHORT>::max() / sizeof(wchar_t)))
    return std::unexpected{make_error(EINVAL)};
  using create_function = NTSTATUS(NTAPI*)(PHANDLE,
                                           ACCESS_MASK,
                                           POBJECT_ATTRIBUTES,
                                           PIO_STATUS_BLOCK,
                                           PLARGE_INTEGER,
                                           ULONG,
                                           ULONG,
                                           ULONG,
                                           ULONG,
                                           PVOID,
                                           ULONG);
  using status_function = ULONG(WINAPI*)(NTSTATUS);

  struct api {
    create_function create{};
    status_function error{};

    api() noexcept {
      const HMODULE module = ::GetModuleHandleW(L"ntdll.dll");
      create = reinterpret_cast<create_function>(::GetProcAddress(module, "NtCreateFile"));
      error = reinterpret_cast<status_function>(::GetProcAddress(module, "RtlNtStatusToDosError"));
    }
  };

  static const api functions;  // 线程安全一次解析，目录每步无需重复查导出。
  if (!functions.create || !functions.error)
    return std::unexpected{make_error(ENOSYS)};
  UNICODE_STRING unicode{};  // Native API 使用显式字节长度，不要求名称有终止符。
  unicode.Buffer = const_cast<wchar_t*>(name.data());
  unicode.Length = unicode.MaximumLength = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = parent;  // 父句柄必须覆盖整个 native 调用。
  attributes.ObjectName = &unicode;
  attributes.Attributes = 0x40;  // OBJ_CASE_INSENSITIVE，符合 Win32 默认名称语义。
  IO_STATUS_BLOCK completion{};
  HANDLE handle = INVALID_HANDLE_VALUE;
  const NTSTATUS status =
      functions.create(&handle,
                       access | SYNCHRONIZE,
                       &attributes,
                       &completion,
                       nullptr,
                       FILE_ATTRIBUTE_NORMAL,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       1,  // FILE_OPEN：绝不因竞争而新建条目。
                       0x20 | 0x00200000,
                       // FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT。
                       nullptr,
                       0);
  if (status < 0)
    return std::unexpected{io::windows::make_windows_error(functions.error(status))};
  return io::windows::owned_file_handle{handle};  // 唯一句柄所有权直接交付。
}

/** @brief 对已经打开的对象设置删除状态；名称替换不会删除替代对象。
 * @details FileDispositionInfo 删除的是句柄所指对象；重解析点本身被删除，
 *          不遍历其目标。只读文件按平台规则返回访问错误，不擅自改变权限。
 */
inline expected<void> delete_handle(HANDLE handle) noexcept {
  FILE_DISPOSITION_INFO disposition{TRUE};
  if (!::SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)))
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  return {};
}
}  // namespace faio::fs::detail
