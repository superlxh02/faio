#pragma once
#include "faio/detail/fs/windows/metadata.hpp"

namespace faio::fs {
/** @brief Windows 文件打开配置；宽字符 CreateFile、共享删除和 OVERLAPPED。 */
class OpenOptions {
 public:
  OpenOptions& read(bool enabled = true) noexcept {
    read_ = enabled;
    return *this;
  }

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
    if (write_ || truncate_)
      access |= GENERIC_WRITE;  // TRUNCATE_EXISTING 要求 FILE_WRITE_DATA。
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
  bool read_{}, write_{}, append_{}, truncate_{}, create_{}, exclusive_{};
  Permissions permissions_{std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
                           | std::filesystem::perms::group_read
                           | std::filesystem::perms::others_read};
};
}  // namespace faio::fs
