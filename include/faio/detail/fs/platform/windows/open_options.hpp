/**
 * @file faio/detail/fs/platform/windows/open_options.hpp
 * @brief 文件打开选项。
 */
#pragma once
#include "faio/detail/fs/platform/windows/metadata.hpp"

namespace faio::fs {
/**  @brief Windows 文件打开配置；宽字符 CreateFile、共享删除和 OVERLAPPED。 */
class open_options {
 public:
  /** @brief 设置打开文件时是否允许读取。 */
  open_options& read(bool enabled = true) noexcept {
    read_ = enabled;
    return *this;
  }

  /** @brief 设置打开文件时是否允许写入。 */
  open_options& write(bool enabled = true) noexcept {
    write_ = enabled;
    return *this;
  }

  /** @brief 设置打开文件时是否将写入追加到末尾。 */
  open_options& append(bool enabled = true) noexcept {
    append_ = enabled;
    return *this;
  }

  /** @brief 设置打开文件时是否将已有内容截断。 */
  open_options& truncate(bool enabled = true) noexcept {
    truncate_ = enabled;
    return *this;
  }

  /** @brief 设置目标不存在时是否创建文件。 */
  open_options& create(bool enabled = true) noexcept {
    create_ = enabled;
    return *this;
  }

  /** @brief 设置独占创建，目标已存在时打开失败。 */
  open_options& create_new(bool enabled = true) noexcept {
    exclusive_ = enabled;
    return *this;
  }

  /** @brief 设置创建文件时采用的权限值。 */
  open_options& creation_permissions(::faio::fs::permissions value) noexcept {
    permissions_ = value;
    return *this;
  }

  /**  @brief 校验权限组合；在 Windows 上返回访问权限掩码，不模拟 POSIX O_* 标志。 */
  expected<int> flags() const noexcept {
    if ((!read_ && !write_ && !append_)
        || ((truncate_ || create_ || exclusive_) && !write_ && !append_))
      return std::unexpected{make_error(EINVAL)};
    DWORD access = FILE_READ_ATTRIBUTES;
    if (write_ || append_)
      access |= FILE_WRITE_ATTRIBUTES;
    if (read_)
      access |= GENERIC_READ;
    if (write_ || truncate_)
      access |= GENERIC_WRITE;
    if (append_)
      access |= FILE_APPEND_DATA;
    return static_cast<int>(access);
  }

  /** @brief 将创建和截断选项转换为 Windows 打开方式。 */
  DWORD disposition() const noexcept {
    if (exclusive_)
      return CREATE_NEW;
    if (create_)
      return truncate_ ? CREATE_ALWAYS : OPEN_ALWAYS;
    return truncate_ ? TRUNCATE_EXISTING : OPEN_EXISTING;
  }

  /** @brief 查询打开选项是否启用追加写入。 */
  bool append_enabled() const noexcept { return append_; }

  /** @brief 返回创建文件时采用的权限配置。 */
  ::faio::fs::permissions permissions() const noexcept { return permissions_; }

 private:
  bool read_{}, write_{}, append_{}, truncate_{}, create_{},
      exclusive_{}; /**< 创建时是否要求目标原先不存在。 */
  ::faio::fs::permissions permissions_{
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
      | std::filesystem::perms::group_read
      | std::filesystem::perms::others_read}; /**< 创建文件或目录时使用的权限配置。 */
};
}  // namespace faio::fs
