/**
 * @file faio/detail/fs/platform/unix/open_options.hpp
 * @brief 文件打开选项。
 */
#pragma once
#include "faio/detail/fs/metadata.hpp"
#include <fcntl.h>

namespace faio::fs {
/**  @brief 具有值语义的文件打开选项；open 支持原生 OPENAT 和有界回退调用。 */
class OpenOptions {
 public:
  /** @brief 设置打开文件时是否允许读取。 */
  OpenOptions& read(bool enabled = true) noexcept {
    read_ = enabled;
    return *this;
  }

  /** @brief 设置打开文件时是否允许写入。 */
  OpenOptions& write(bool enabled = true) noexcept {
    write_ = enabled;
    return *this;
  }

  /** @brief 设置打开文件时是否将写入追加到末尾。 */
  OpenOptions& append(bool enabled = true) noexcept {
    append_ = enabled;
    return *this;
  }

  /** @brief 设置打开文件时是否将已有内容截断。 */
  OpenOptions& truncate(bool enabled = true) noexcept {
    truncate_ = enabled;
    return *this;
  }

  /** @brief 设置目标不存在时是否创建文件。 */
  OpenOptions& create(bool enabled = true) noexcept {
    create_ = enabled;
    return *this;
  }

  /** @brief 设置独占创建，目标已存在时打开失败。 */
  OpenOptions& create_new(bool enabled = true) noexcept {
    exclusive_ = enabled;
    return *this;
  }

  /** @brief 设置创建文件时采用的权限值。 */
  OpenOptions& creation_permissions(Permissions permissions) noexcept {
    permissions_ = permissions;
    return *this;
  }

  /** @brief 验证打开选项并生成原生标志位。 */
  [[nodiscard]] expected<int> flags() const noexcept {
    const bool writable = write_ || append_;
    if ((!read_ && !writable) || ((truncate_ || create_ || exclusive_) && !writable))
      return std::unexpected{make_error(EINVAL)};
    int flags = O_CLOEXEC | (read_ ? (writable ? O_RDWR : O_RDONLY) : O_WRONLY);
    if (append_)
      flags |= O_APPEND;
    if (truncate_ && !exclusive_)
      flags |= O_TRUNC;
    if (create_ || exclusive_)
      flags |= O_CREAT;
    if (exclusive_)
      flags |= O_EXCL;
    return flags;
  }

  /** @brief 查询打开选项是否启用追加写入。 */
  [[nodiscard]] bool append_enabled() const noexcept { return append_; }

  /** @brief 返回创建文件时采用的权限配置。 */
  [[nodiscard]] Permissions permissions() const noexcept { return permissions_; }

 private:
  bool read_{}, write_{}, append_{}, truncate_{}, create_{},
      exclusive_{}; /**< 创建时是否要求目标原先不存在。 */
  Permissions permissions_{
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
      | std::filesystem::perms::group_read
      | std::filesystem::perms::others_read}; /**< 创建文件或目录时使用的权限配置。 */
};
}  // namespace faio::fs
