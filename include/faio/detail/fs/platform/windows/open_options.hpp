/**
 * @file faio/detail/fs/platform/windows/open_options.hpp
 * @brief 文件与目录异步接口：文件打开选项。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#include "faio/detail/fs/platform/windows/metadata.hpp"

namespace faio::fs {
/** @brief Windows 文件打开配置；宽字符 CreateFile、共享删除和 OVERLAPPED。 */
class OpenOptions {
 public:
  /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
  OpenOptions& read(bool enabled = true) noexcept {
    read_ = enabled;
    return *this;
  }

  /// @brief 发起一次写入或发送，可能只完成部分字节，调用方须按返回进度继续。
  OpenOptions& write(bool enabled = true) noexcept {
    write_ = enabled;
    return *this;
  }

  OpenOptions& append(bool enabled = true) noexcept {
    append_ = enabled;
    return *this;
  }

  OpenOptions& truncate(bool enabled = true) noexcept {
    truncate_ = enabled;
    return *this;
  }

  OpenOptions& create(bool enabled = true) noexcept {
    create_ = enabled;
    return *this;
  }

  OpenOptions& create_new(bool enabled = true) noexcept {
    exclusive_ = enabled;
    return *this;
  }

  OpenOptions& creation_permissions(Permissions value) noexcept {
    permissions_ = value;
    return *this;
  }

  /** @brief 校验权限组合；Windows 返回 access mask，而不是伪造 POSIX O_*。 */
  expected<int> flags() const noexcept {
    if ((!read_ && !write_ && !append_)
        || ((truncate_ || create_ || exclusive_) && !write_ && !append_))
      return std::unexpected{make_error(EINVAL)};
    DWORD access = FILE_READ_ATTRIBUTES;
    if (write_ || append_)
      access |= FILE_WRITE_ATTRIBUTES;
    if (read_)
      access |= GENERIC_READ;
    /// @brief TRUNCATE_EXISTING 要求 FILE_WRITE_DATA。
    if (write_ || truncate_)
      access |= GENERIC_WRITE;
    if (append_)
      access |= FILE_APPEND_DATA;
    return static_cast<int>(access);
  }

  DWORD disposition() const noexcept {
    if (exclusive_)
      return CREATE_NEW;
    if (create_)
      return truncate_ ? CREATE_ALWAYS : OPEN_ALWAYS;
    return truncate_ ? TRUNCATE_EXISTING : OPEN_EXISTING;
  }

  bool append_enabled() const noexcept { return append_; }

  Permissions permissions() const noexcept { return permissions_; }

 private:
  bool read_{},  ///< 打开时是否允许读取。
      write_{},  ///< 打开时是否允许写入。
      append_{},  ///< 打开时是否启用追加写入。
      truncate_{},  ///< 打开时是否截断已有文件。
      create_{},  ///< 路径不存在时是否允许创建文件。
      exclusive_{};  ///< 创建时是否要求目标原先不存在。
  Permissions permissions_{std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
                           | std::filesystem::perms::group_read
                           | std::filesystem::perms::others_read};  ///< 创建文件或目录时使用的权限配置。
};
}
