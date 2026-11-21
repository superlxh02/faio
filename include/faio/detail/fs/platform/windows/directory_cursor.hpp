#pragma once
/**
 * @file faio/detail/fs/platform/windows/directory_cursor.hpp
 * @brief Windows 固定目录 HANDLE 的有界批量枚举。
 */
#include "faio/detail/fs/platform/windows/metadata.hpp"
#include "faio/detail/io/core/context.hpp"
#include <array>
#include <cstring>
#include <optional>

namespace faio::fs::detail {
/**  @brief 一次原生枚举最多 16 KiB，take 只解析缓存，不调用系统枚举。 */
struct native_directory_cursor {
  /** @brief 原生目录枚举返回的名称和文件类型记录。 */
  struct entry {
    std::wstring name;  /**< 持有目录项名称，避免借用会被下一批枚举覆盖的存储。 */
    DWORD attributes{}; /**< Windows 原生文件属性，用于分类与权限映射。 */
  };

  static constexpr std::size_t buffer_size = 16 * 1024; /**< 原生目录枚举缓冲区的固定字节容量。 */
  io::io_context context;                               /**< 析构固定交给创建时的清理通道。 */
  io::windows::owned_file_handle handle; /**< 指向目录对象的句柄，重命名不改变枚举锚点。 */
  alignas(FILE_ID_BOTH_DIR_INFO) std::array<
      std::byte,
      buffer_size> buffer{}; /**< 对象持有的字节缓冲区，在异步挂起期间保持内容并满足地址要求。 */
  std::size_t position{buffer_size}; /**< 缓存耗尽时使用固定哨兵，避免解析旧批次。 */
  bool eof{};                        /**< 目录或输入是否已到末尾，后续读取据此立即返回。 */

  /** @brief 初始化原生目录枚举游标。 */
  native_directory_cursor(io::io_context owner, HANDLE directory)
      : context(std::move(owner)), handle(directory) {}

  /** @brief 禁止复制此对象。 */
  native_directory_cursor(const native_directory_cursor&) = delete;

  /** @brief 关闭原生目录搜索句柄。 */
  ~native_directory_cursor() {
    const HANDLE directory = handle.release();
    if (valid_handle(directory))
      context.defer_cleanup([directory] { ::CloseHandle(directory); });
  }

  /** @brief 移交目录句柄的关闭责任，并将原句柄置空。 */
  HANDLE release() noexcept { return handle.release(); }

  /**  @brief 仅在独立的文件服务中填充缓存；Windows 没有用于目录枚举的 IOCP 操作码。 */
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
    position = 0;
    return {};
  }

  /**  @brief 返回独立持有的名称和属性，校验变长结构的边界，并避免违反别名规则。 */
  expected<std::optional<entry>> take() {
    constexpr std::size_t header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
    while (position != buffer_size) {
      if (position > buffer_size - header)
        return std::unexpected{make_error(EIO)};
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
      std::memcpy(name.data(), record + header, name_bytes);
      position = next ? position + next : buffer_size;
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
