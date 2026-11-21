#pragma once
/**
 * @file faio/detail/fs/platform/windows/native.hpp
 * @brief Windows 文件系统的 Unicode、句柄与属性适配。
 */
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/platform/windows/handle.hpp"
#include "faio/detail/io/platform/windows/error.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <winternl.h>

namespace faio::fs::detail {
/**
 *  @brief 把路径固定为绝对 UTF-16 扩展路径，支持中文、UNC 和长路径。
 * @details 不经过 ANSI 代码页；已有设备路径保持原表示。参数由任务拥有，
 *          系统调用期间不会借用调用者的临时 string/path。
 */
inline expected<std::wstring> native_path(const std::filesystem::path& path) {
  const auto& name = path.native();
  if (name.empty() || name.find(L'\0') != std::wstring::npos)
    return std::unexpected{make_error(EINVAL)};
  if (name.starts_with(L"\\\\?\\") || name.starts_with(L"\\\\.\\"))
    return name;
  DWORD required = ::GetFullPathNameW(name.c_str(), 0, nullptr, nullptr);
  if (!required)
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  std::wstring result(required, L'\0');
  for (;;) {
    const DWORD length =
        ::GetFullPathNameW(name.c_str(), static_cast<DWORD>(result.size()), result.data(), nullptr);
    if (!length)
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    if (length < result.size()) {
      result.resize(length);
      break;
    }
    result.resize(static_cast<std::size_t>(length) + 1);
  }
  std::replace(result.begin(), result.end(), L'/', L'\\');
  if (result.starts_with(L"\\\\"))
    return L"\\\\?\\UNC\\" + result.substr(2);
  return L"\\\\?\\" + result;
}

/**  @brief 检查 HANDLE 是否有效，避免向内核提交 NULL 或 INVALID_HANDLE_VALUE。 */
inline bool valid_handle(HANDLE handle) noexcept {
  return handle && handle != INVALID_HANDLE_VALUE;
}

/**
 *  @brief 使用已固定的父目录句柄打开一个直接子项，不跟随重解析点。
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

  /** @brief ntdll 文件接口的动态入口表。 */
  struct api {
    create_function create{}; /**< 动态解析的原生文件创建入口，未提供时采用兼容路径。 */
    status_function error{};  /**< 将 NTSTATUS 转换为 Win32 错误码的动态入口。 */

    /** @brief 解析 NtCreateFile 和 NTSTATUS 转换函数的入口。 */
    api() noexcept {
      const HMODULE module = ::GetModuleHandleW(L"ntdll.dll");
      create = reinterpret_cast<create_function>(::GetProcAddress(module, "NtCreateFile"));
      error = reinterpret_cast<status_function>(::GetProcAddress(module, "RtlNtStatusToDosError"));
    }
  };

  static const api functions;
  if (!functions.create || !functions.error)
    return std::unexpected{make_error(ENOSYS)};
  UNICODE_STRING unicode{};
  unicode.Buffer = const_cast<wchar_t*>(name.data());
  unicode.Length = unicode.MaximumLength = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = parent;
  attributes.ObjectName = &unicode;
  attributes.Attributes = 0x40;
  IO_STATUS_BLOCK completion{};
  HANDLE handle = INVALID_HANDLE_VALUE;
  const NTSTATUS status = functions.create(&handle,
                                           access | SYNCHRONIZE,
                                           &attributes,
                                           &completion,
                                           nullptr,
                                           FILE_ATTRIBUTE_NORMAL,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           1,
                                           0x20 | 0x00200000,
                                           nullptr,
                                           0);
  if (status < 0)
    return std::unexpected{io::windows::make_windows_error(functions.error(status))};
  return io::windows::owned_file_handle{handle};
}

/**
 *  @brief 对已经打开的对象设置删除状态；名称替换不会删除替代对象。
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
