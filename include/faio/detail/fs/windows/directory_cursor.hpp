#pragma once
/** @file directory_cursor.hpp @brief Windows 固定目录 HANDLE 的有界批量枚举。 */
#include "faio/detail/fs/windows/metadata.hpp"
#include "faio/detail/io/context.hpp"
#include <array>
#include <cstring>
#include <optional>

namespace faio::fs::detail {
/** @brief 一次原生枚举最多 16 KiB，take 只解析缓存，不调用系统枚举。 */
struct native_directory_cursor {
  struct entry {
    std::wstring name;
    DWORD attributes{};
  };

  static constexpr std::size_t buffer_size = 16 * 1024;
  io::io_context context;                 ///< 析构固定交给创建时的清理通道。
  io::windows::owned_file_handle handle;  ///< 目录真实对象，重命名不改变枚举锚点。
  alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, buffer_size> buffer{};
  std::size_t position{buffer_size};  ///< 耗尽使用固定哨兵，避免解析旧批次。
  bool eof{};

  native_directory_cursor(io::io_context owner, HANDLE directory)
      : context(std::move(owner)), handle(directory) {}

  native_directory_cursor(const native_directory_cursor&) = delete;

  ~native_directory_cursor() {
    const HANDLE directory = handle.release();
    if (valid_handle(directory))
      context.defer_cleanup([directory] { ::CloseHandle(directory); });
  }

  HANDLE release() noexcept { return handle.release(); }

  /** @brief 只在隔离文件服务填充；Windows 无目录枚举 IOCP opcode。 */
  expected<void> fill() noexcept {
    if (position != buffer_size || eof)
      return {};
    if (!::GetFileInformationByHandleEx(handle.get(),
                                        FileIdBothDirectoryInfo,
                                        buffer.data(),
                                        static_cast<DWORD>(buffer.size()))) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_NO_MORE_FILES) {
        eof = true;
        return {};
      }
      return std::unexpected{io::windows::make_windows_error(error)};
    }
    position = 0;  // 成功批次第一条记录，短批次仍允许下一次 fill。
    return {};
  }

  /** @brief 返回拥有型名称和属性；验证变长结构边界且不使用别名违规访问。 */
  expected<std::optional<entry>> take() {
    constexpr std::size_t header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
    while (position != buffer_size) {
      if (position > buffer_size - header)
        return std::unexpected{make_error(EIO)};
      // 不直接 reinterpret_cast 非对象缓存；固定字段由 memcpy 提取。
      DWORD next{}, attributes{}, name_bytes{};
      const std::byte* record = buffer.data() + position;
      std::memcpy(&next, record + offsetof(FILE_ID_BOTH_DIR_INFO, NextEntryOffset), sizeof(next));
      std::memcpy(&attributes,
                  record + offsetof(FILE_ID_BOTH_DIR_INFO, FileAttributes),
                  sizeof(attributes));
      std::memcpy(&name_bytes,
                  record + offsetof(FILE_ID_BOTH_DIR_INFO, FileNameLength),
                  sizeof(name_bytes));
      const std::size_t available = buffer_size - position;
      if (name_bytes % sizeof(wchar_t) || name_bytes > available - header
          || (next && (next < header + name_bytes || next > available || next % 8)))
        return std::unexpected{make_error(EIO)};
      std::wstring name(name_bytes / sizeof(wchar_t), L'\0');
      std::memcpy(name.data(), record + header, name_bytes);  // 复制后可安全覆盖原缓存。
      position = next ? position + next : buffer_size;        // 零偏移明确标记本批结束。
      if (name == L"." || name == L"..")
        continue;
      if (name.empty() || name.find(L'\0') != std::wstring::npos)
        return std::unexpected{make_error(EIO)};
      return std::optional<entry>{entry{std::move(name), attributes}};
    }
    return std::optional<entry>{};
  }
};
}  // namespace faio::fs::detail
