#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/windows/open_options.hpp"
#else
#include "faio/detail/fs/metadata.hpp"
#include <fcntl.h>

namespace faio::fs {
/** @brief 拥有值语义的文件打开选项；open支持原生OPENAT和有界fallback。 */
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

  OpenOptions& creation_permissions(Permissions permissions) noexcept {
    permissions_ = permissions;
    return *this;
  }

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

  [[nodiscard]] bool append_enabled() const noexcept { return append_; }

  [[nodiscard]] Permissions permissions() const noexcept { return permissions_; }

 private:
  bool read_{}, write_{}, append_{}, truncate_{}, create_{}, exclusive_{};
  Permissions permissions_{std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
                           | std::filesystem::perms::group_read
                           | std::filesystem::perms::others_read};
};
}  // namespace faio::fs

#endif  // _WIN32
