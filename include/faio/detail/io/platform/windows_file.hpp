#pragma once
/** @file windows_file.hpp @brief 低层文件接口的 UTF-8 路径和有界服务适配。 */
#include "faio/detail/io/operation.hpp"
#include "faio/detail/io/platform/windows_error.hpp"
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <new>
#include <stdexcept>
#include <winternl.h>

namespace faio::io::windows {
/** @brief 固定条带锁保护低层隐式 OS 游标，没有随 HANDLE 累积的全局缓存。
 * @details 文件标识使 DuplicateHandle 和不同 IO 域的同一文件共用锁；不同
 *          FILE_OBJECT 虽可能保守串行，但各自 OS 游标仍独立。高层 File 的
 *          显式位置 IO 不使用本锁，也不走此服务路径。
 */
inline std::mutex& implicit_cursor_mutex(HANDLE handle) noexcept {
  static std::array<std::mutex, 64> stripes;
  BY_HANDLE_FILE_INFORMATION identity{};
  std::uint64_t key = reinterpret_cast<std::uintptr_t>(handle);
  if (::GetFileInformationByHandle(handle, &identity)) {
    key = (static_cast<std::uint64_t>(identity.nFileIndexHigh) << 32) | identity.nFileIndexLow;
    key ^= static_cast<std::uint64_t>(identity.dwVolumeSerialNumber) << 17;
  }
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdULL;
  key ^= key >> 33;
  return stripes[static_cast<std::size_t>(key) & (stripes.size() - 1)];
}

/** @brief 查询真实句柄是否只有追加权限，支持借用的外部 CreateFile 句柄。 */
inline expected<bool> append_only_handle(HANDLE handle) noexcept {
  using query_fn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  using error_fn = ULONG(WINAPI*)(NTSTATUS);
  static auto query = reinterpret_cast<query_fn>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
  static auto translate = reinterpret_cast<error_fn>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
  if (!query || !translate)
    return std::unexpected{make_windows_error(ERROR_PROC_NOT_FOUND)};

  struct basic_information {
    // PUBLIC_OBJECT_BASIC_INFORMATION 的公开固定布局。
    ULONG attributes{};
    ACCESS_MASK granted_access{};
    ULONG handle_count{}, pointer_count{}, reserved[10]{};
  } information;

  const NTSTATUS status = query(handle,
                                0,
                                &information,
                                sizeof(information),
                                nullptr);  // ObjectBasicInformation。
  if (status < 0)
    return std::unexpected{make_windows_error(translate(status))};
  const ACCESS_MASK access =
      information.granted_access;  // 内核对象查询没有磁盘 IO/STATUS_PENDING 生命周期。
  return (access & FILE_APPEND_DATA) && !(access & FILE_WRITE_DATA);
}

/** @brief 文件 lane 的私有 OVERLAPPED；hEvent 低位禁止产生孤立 IOCP 完成包。 */
inline std::int64_t file_transfer(HANDLE handle,
                                  HANDLE event,
                                  void* buffer,
                                  std::size_t size,
                                  std::uint64_t offset,
                                  bool writing) noexcept {
  if (size && !buffer)
    return -EFAULT;
  const DWORD length = static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  overlapped.hEvent = reinterpret_cast<HANDLE>(reinterpret_cast<std::uintptr_t>(event) | 1);
  ::ResetEvent(event);  // 每段复用事件之前清除上一段的完成信号。
  DWORD bytes{};
  BOOL completed = writing ? ::WriteFile(handle, buffer, length, &bytes, &overlapped)
                           : ::ReadFile(handle, buffer, length, &bytes, &overlapped);
  DWORD error = completed ? ERROR_SUCCESS : ::GetLastError();
  if (error == ERROR_IO_PENDING) {
    completed = ::GetOverlappedResult(handle,
                                      &overlapped,
                                      &bytes,
                                      TRUE);  // 借用排空以后才返回 lane。
    error = completed ? ERROR_SUCCESS : ::GetLastError();
  }
  if (error == ERROR_HANDLE_EOF && !writing)
    return 0;
  return error ? -encode_windows_error(error) : static_cast<std::int64_t>(bytes);
}

/** @brief OS 游标快照/更新；零位移查询不会修改文件数据或逻辑游标。 */
inline std::int64_t query_file_position(HANDLE handle, std::uint64_t& position) noexcept {
  LARGE_INTEGER zero{}, current{};
  if (!::SetFilePointerEx(handle, zero, &current, FILE_CURRENT))
    return -encode_windows_error(::GetLastError());
  if (current.QuadPart < 0)
    return -EINVAL;
  position = static_cast<std::uint64_t>(current.QuadPart);
  return 0;
}

inline std::int64_t update_file_position(HANDLE handle, std::uint64_t position) noexcept {
  if (position > static_cast<std::uint64_t>(INT64_MAX))
    return -EOVERFLOW;
  LARGE_INTEGER next{};
  next.QuadPart = static_cast<LONGLONG>(position);
  return ::SetFilePointerEx(handle, next, nullptr, FILE_BEGIN)
             ? 0
             : -encode_windows_error(::GetLastError());
}

/** @brief 低层默认 read/write 使用并推进 OS 游标；不将 UINT64_MAX 传给
 * ReadFile。 */
inline std::int64_t scalar_file(detail::io_request& request) noexcept {
  HANDLE handle = reinterpret_cast<HANDLE>(request.fd);
  const bool writing = request.kind == detail::operation_kind::write;
  owned_file_handle event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!event)
    return -encode_windows_error(::GetLastError());
  if (!request.windows_implicit_cursor)
    return file_transfer(handle,
                         event.get(),
                         writing ? const_cast<void*>(request.const_buffer) : request.buffer,
                         request.length,
                         request.offset,
                         writing);
  const DWORD type = ::GetFileType(handle);
  if (type != FILE_TYPE_DISK)  // 管道/设备没有 seek 游标，OVERLAPPED offset 对它们无效。
    return file_transfer(handle,
                         event.get(),
                         writing ? const_cast<void*>(request.const_buffer) : request.buffer,
                         request.length,
                         0,
                         writing);
  std::lock_guard lock(implicit_cursor_mutex(handle));  // 跨查询、真实 IO 和更新保持不可分割。
  std::uint64_t position{};
  if (auto error = query_file_position(handle, position); error)
    return error;
  bool append = false;
  if (writing) {
    auto access = append_only_handle(handle);
    if (!access)
      return -encode_windows_error(static_cast<std::uint32_t>(access.error().value()));
    append = *access;
  }
  const auto count =
      file_transfer(handle,
                    event.get(),
                    writing ? const_cast<void*>(request.const_buffer) : request.buffer,
                    request.length,
                    append ? UINT64_MAX : position,
                    writing);
  if (count < 0)
    return count;
  if (append && count) {
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(handle, &size))
      position = static_cast<std::uint64_t>(size.QuadPart);
    else
      return count ? count : -encode_windows_error(::GetLastError());  // 已传输进度不能丢失。
  } else
    position += static_cast<std::uint64_t>(count);
  const auto error = update_file_position(handle, position);
  return error && !count ? error : count;  // 已生效的字节按 POSIX 部分成功交付。
}

/** @brief 在文件 lane 将严格 UTF-8 路径打开为 OVERLAPPED HANDLE。
 * @details 不使用依赖 CRT 代码页的 path(char*) 转换。该边界为 noexcept，
 *          拒绝截断路径、未知 flags 和文本转换；服务线程从不因路径异常退出。
 */
inline std::int64_t open_file(detail::io_request& request) noexcept {
  try {
    if (request.path.empty() || request.path.find('\0') != std::string::npos)
      return -EINVAL;  // 内部拥有字符串也不能让 Win32 按 NUL 截断成另一个对象。
    if (request.path.size() > INT_MAX)
      return -ENAMETOOLONG;
    const int characters = ::MultiByteToWideChar(CP_UTF8,
                                                 MB_ERR_INVALID_CHARS,
                                                 request.path.data(),
                                                 static_cast<int>(request.path.size()),
                                                 nullptr,
                                                 0);
    if (!characters)
      return -encode_windows_error(::GetLastError());
    std::wstring path(static_cast<std::size_t>(characters), L'\0');
    if (!::MultiByteToWideChar(CP_UTF8,
                               MB_ERR_INVALID_CHARS,
                               request.path.data(),
                               static_cast<int>(request.path.size()),
                               path.data(),
                               characters))
      return -encode_windows_error(::GetLastError());
    std::replace(path.begin(), path.end(), L'/', L'\\');
    const int flags = request.flags;
    constexpr int supported = _O_WRONLY | _O_RDWR | _O_APPEND | _O_CREAT | _O_TRUNC | _O_EXCL
                              | _O_BINARY | _O_NOINHERIT | _O_RANDOM | _O_SEQUENTIAL | _O_TEMPORARY
                              | _O_SHORT_LIVED
#ifdef _O_OBTAIN_DIR
                              | _O_OBTAIN_DIR
#endif
        ;
    if (flags & ~supported)
      return -EOPNOTSUPP;  // 文本/UTF 文本、POSIX
    // 扩展不能静默降成普通二进制打开。
    const int access_mode = flags & (_O_WRONLY | _O_RDWR);
    if (access_mode == (_O_WRONLY | _O_RDWR) || ((flags & _O_TRUNC) && !access_mode)
        || ((flags & _O_RANDOM) && (flags & _O_SEQUENTIAL)))
      return -EINVAL;
    DWORD access = (flags & _O_RDWR)     ? GENERIC_READ | GENERIC_WRITE
                   : (flags & _O_WRONLY) ? GENERIC_WRITE
                                         : GENERIC_READ;
    const bool append = (flags & _O_APPEND) && access_mode;
    if (append) {
      access &= ~GENERIC_WRITE;
      access |= FILE_APPEND_DATA | FILE_READ_ATTRIBUTES;  // 查询 EOF/文件标识不扩大写数据权限。
    }
    if (flags & _O_TEMPORARY)
      access |= DELETE;
    const DWORD initial_access = access | ((append && (flags & _O_TRUNC)) ? GENERIC_WRITE : 0);
    DWORD attributes = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_BACKUP_SEMANTICS;
    if (flags & _O_RANDOM)
      attributes |= FILE_FLAG_RANDOM_ACCESS;
    if (flags & _O_SEQUENTIAL)
      attributes |= FILE_FLAG_SEQUENTIAL_SCAN;
    if (flags & _O_SHORT_LIVED)
      attributes = (attributes & ~FILE_ATTRIBUTE_NORMAL) | FILE_ATTRIBUTE_TEMPORARY;
    if (flags & _O_TEMPORARY)
      attributes |= FILE_FLAG_DELETE_ON_CLOSE;
    if ((flags & _O_CREAT) && !(request.argument & 0200))
      attributes |= FILE_ATTRIBUTE_READONLY;  // 仅新建对象采用 mode
    // 的只读位，ACL 仍由系统继承。
    const auto finish_open = [=](HANDLE value) noexcept -> std::int64_t {
      owned_file_handle handle{value};  // 失败路径也只关闭一次临时写数据句柄。
      if (initial_access != access) {
        HANDLE narrowed =
            ::ReOpenFile(handle.get(),
                         access,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         attributes & 0xffff0000u);  // ReOpenFile 禁止 FILE_ATTRIBUTE_*。
        if (narrowed == INVALID_HANDLE_VALUE)
          return -encode_windows_error(::GetLastError());
        handle.reset(narrowed);  // truncate 完成以后最终句柄仍只有追加权限。
      }
      return reinterpret_cast<std::intptr_t>(handle.release());
    };
    DWORD disposition = OPEN_EXISTING;
    if (flags & _O_CREAT)
      disposition = (flags & _O_EXCL)    ? CREATE_NEW
                    : (flags & _O_TRUNC) ? CREATE_ALWAYS
                                         : OPEN_ALWAYS;
    else if (flags & _O_TRUNC)
      disposition = TRUNCATE_EXISTING;
    const bool rooted = path.front() == L'\\' || (path.size() > 1 && path[1] == L':');
    if (request.fd != AT_FDCWD && !rooted) {
      // 父目录 HANDLE 的相对打开不能转换为拼接路径，否则 rename 会改变目标。
      using create_fn = NTSTATUS(NTAPI*)(PHANDLE,
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
      using error_fn = ULONG(WINAPI*)(NTSTATUS);
      static auto create = reinterpret_cast<create_fn>(
          ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
      static auto translate = reinterpret_cast<error_fn>(
          ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
      if (!create || !translate)
        return -encode_windows_error(ERROR_PROC_NOT_FOUND);
      if (path.size() > USHRT_MAX / sizeof(wchar_t))
        return -ENAMETOOLONG;
      UNICODE_STRING name{static_cast<USHORT>(path.size() * sizeof(wchar_t)),
                          static_cast<USHORT>(path.size() * sizeof(wchar_t)),
                          path.data()};
      OBJECT_ATTRIBUTES attributes{};
      attributes.Length = sizeof(attributes);
      attributes.RootDirectory = reinterpret_cast<HANDLE>(request.fd);
      attributes.ObjectName = &name;
      attributes.Attributes = 0x40;  // OBJ_CASE_INSENSITIVE。
      IO_STATUS_BLOCK status{};
      HANDLE handle{};
      ULONG native_disposition = disposition == CREATE_NEW          ? 2
                                 : disposition == CREATE_ALWAYS     ? 5
                                 : disposition == OPEN_ALWAYS       ? 3
                                 : disposition == TRUNCATE_EXISTING ? 4
                                                                    : 1;
      ULONG native_options = (flags & _O_RANDOM ? 0x800 : 0) | (flags & _O_SEQUENTIAL ? 0x4 : 0)
                             | (flags & _O_TEMPORARY ? 0x1000 : 0);
      NTSTATUS result = create(
          &handle,
          initial_access | SYNCHRONIZE,
          &attributes,
          &status,
          nullptr,
          (flags & _O_SHORT_LIVED ? FILE_ATTRIBUTE_TEMPORARY : FILE_ATTRIBUTE_NORMAL)
              | ((flags & _O_CREAT) && !(request.argument & 0200) ? FILE_ATTRIBUTE_READONLY : 0),
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          native_disposition,
          native_options,
          nullptr,
          0);
      return result < 0 ? -encode_windows_error(translate(result)) : finish_open(handle);
    }
    if (!path.starts_with(L"\\\\?\\") && !path.starts_with(L"\\\\.\\")) {
      DWORD required = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
      if (!required)
        return -encode_windows_error(::GetLastError());
      std::wstring absolute(required, L'\0');
      for (;;) {
        DWORD length = ::GetFullPathNameW(
            path.c_str(), static_cast<DWORD>(absolute.size()), absolute.data(), nullptr);
        if (!length)
          return -encode_windows_error(::GetLastError());
        if (length < absolute.size()) {
          absolute.resize(length);
          break;
        }
        absolute.resize(static_cast<std::size_t>(length)
                        + 1);  // 并发改变 CWD 时只增加容量，直到一次转换容纳完整路径。
      }
      path = std::move(absolute);
      if (path.starts_with(L"\\\\"))
        path = L"\\\\?\\UNC\\" + path.substr(2);
      else
        path = L"\\\\?\\" + path;
    }
    HANDLE handle = ::CreateFileW(path.c_str(),
                                  initial_access,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  disposition,
                                  attributes,
                                  nullptr);
    return handle == INVALID_HANDLE_VALUE ? -encode_windows_error(::GetLastError())
                                          : finish_open(handle);
  } catch (const std::bad_alloc&) {
    return -ENOMEM;  // noexcept 服务边界不能把内存压力变成线程/进程退出。
  } catch (const std::filesystem::filesystem_error& error) {
    return error.code().category() == std::system_category()
               ? -encode_windows_error(static_cast<std::uint32_t>(error.code().value()))
               : -error.code().value();
  } catch (const std::length_error&) {
    return -ENAMETOOLONG;
  } catch (const std::invalid_argument&) {
    return -EINVAL;
  } catch (...) {
    return -EIO;  // 未知转换异常也必须转换成完成结果，不能穿过 noexcept domain。
  }
}

/** @brief 分散文件 IO fallback；只在独立文件 lane 阻塞，不占用协程 worker。
 * @details Windows ReadFileScatter 对页对齐有特殊限制，通用 iovec 按元素执行。
 *          hEvent 低位禁止向 IOCP 投递这些 lane 私有操作，避免双重完成。
 */
inline std::int64_t vectored_file(detail::io_request& request) noexcept {
  if (request.flags)
    return -EOPNOTSUPP;
  HANDLE handle = reinterpret_cast<HANDLE>(request.fd);
  const bool writing = request.kind == detail::operation_kind::writev;
  owned_file_handle event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!event)
    return -encode_windows_error(::GetLastError());
  const bool tracked = request.windows_implicit_cursor && ::GetFileType(handle) == FILE_TYPE_DISK;
  std::unique_lock<std::mutex> lock;  // 整个 vector 数组共用同一次游标事务。
  std::uint64_t offset = request.offset, total{};
  bool append = false;
  if (tracked) {
    lock = std::unique_lock<std::mutex>{implicit_cursor_mutex(handle)};
    if (auto error = query_file_position(handle, offset); error)
      return error;
    if (writing) {
      auto access = append_only_handle(handle);
      if (!access)
        return -encode_windows_error(static_cast<std::uint32_t>(access.error().value()));
      append = *access;
    }
  } else if (request.windows_implicit_cursor)
    offset = 0;  // 无 seek 游标的设备忽略位置字段。
  std::int64_t error{};
  for (const auto& vector : request.vectors) {
    if (!vector.iov_len)
      continue;
    const auto bytes = file_transfer(handle,
                                     event.get(),
                                     vector.iov_base,
                                     vector.iov_len,
                                     append ? UINT64_MAX : offset,
                                     writing);
    if (bytes < 0) {
      error = bytes;
      break;
    }  // 后续段失败保留已经生效的前缀。
    total += bytes;
    if (!append)
      offset += bytes;
    if (static_cast<std::uint64_t>(bytes) < vector.iov_len)
      break;
  }
  if (tracked) {
    if (append && total) {
      LARGE_INTEGER size{};
      if (!::GetFileSizeEx(handle, &size))
        return static_cast<std::int64_t>(total);
      offset = static_cast<std::uint64_t>(size.QuadPart);  // 只有真正追加权限才采用最新 EOF。
    }
    const auto update = update_file_position(handle, offset);
    if (!error)
      error = update;
  }
  return total ? static_cast<std::int64_t>(total)
               : error;  // EOF/全空数组返回零，失败前缀按部分成功返回。
}
}  // namespace faio::io::windows
