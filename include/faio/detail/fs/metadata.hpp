#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/windows/metadata.hpp"
#else
#include "faio/detail/common/error.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <sys/stat.h>
#if defined(__linux__)
#include <linux/stat.h>
#include <sys/sysmacros.h>
#endif

namespace faio::fs {
using path = std::filesystem::path;

/** @brief 便携权限值；POSIX 创建模式扩展不进入通用构建器。 */
class Permissions {
 public:
  explicit Permissions(std::filesystem::perms bits = std::filesystem::perms::unknown) noexcept
      : bits_(bits) {}

  [[nodiscard]] std::filesystem::perms bits() const noexcept { return bits_; }

  [[nodiscard]] bool readonly() const noexcept {
    return (bits_
            & (std::filesystem::perms::owner_write | std::filesystem::perms::group_write
               | std::filesystem::perms::others_write))
           == std::filesystem::perms::none;
  }

  void set_readonly(bool value) noexcept {
    constexpr auto write_bits = std::filesystem::perms::owner_write
                                | std::filesystem::perms::group_write
                                | std::filesystem::perms::others_write;
    if (value)
      bits_ &= ~write_bits;
    else
      bits_ |= std::filesystem::perms::owner_write;
  }

 private:
  std::filesystem::perms bits_;
};

/** @brief 一次原生STATX或stat/lstat/fstat得到的拥有型属性快照。 */
class Metadata {
 public:
  explicit Metadata(struct stat value) noexcept : value_(value) {}
#if defined(__linux__)
  /** @brief 将原生STATX_BASIC_STATS结果转换为统一属性，不再次调用stat。
   * @details 保留设备/inode身份、权限、长度与纳秒时间；转换只读取CQE已经完成的
   *          输出内存。设备号使用系统makedev编码，与fstat的same_file兼容。
   */
  explicit Metadata(const struct statx& value) noexcept : value_{} {
    value_.st_dev = ::makedev(value.stx_dev_major, value.stx_dev_minor);
    value_.st_ino = value.stx_ino;
    value_.st_nlink = value.stx_nlink;
    value_.st_mode = value.stx_mode;
    value_.st_uid = value.stx_uid;
    value_.st_gid = value.stx_gid;
    value_.st_rdev = ::makedev(value.stx_rdev_major, value.stx_rdev_minor);
    value_.st_size = static_cast<off_t>(value.stx_size);
    value_.st_blksize = value.stx_blksize;
    value_.st_blocks = value.stx_blocks;
    value_.st_atim = {static_cast<time_t>(value.stx_atime.tv_sec),
                      static_cast<long>(value.stx_atime.tv_nsec)};
    value_.st_mtim = {static_cast<time_t>(value.stx_mtime.tv_sec),
                      static_cast<long>(value.stx_mtime.tv_nsec)};
    value_.st_ctim = {static_cast<time_t>(value.stx_ctime.tv_sec),
                      static_cast<long>(value.stx_ctime.tv_nsec)};
  }
#endif
  [[nodiscard]] std::uint64_t len() const noexcept {
    return static_cast<std::uint64_t>(value_.st_size);
  }

  [[nodiscard]] bool is_file() const noexcept { return S_ISREG(value_.st_mode); }

  [[nodiscard]] bool is_dir() const noexcept { return S_ISDIR(value_.st_mode); }

  [[nodiscard]] bool is_symlink() const noexcept { return S_ISLNK(value_.st_mode); }

  [[nodiscard]] Permissions permissions() const noexcept {
    return Permissions{static_cast<std::filesystem::perms>(value_.st_mode & 07777)};
  }

  /** @brief
   * 两次POSIX属性快照是否指向同一设备上的同一inode，用于防止自复制截断。 */
  [[nodiscard]] bool same_file(const Metadata& other) const noexcept {
    return value_.st_dev == other.value_.st_dev && value_.st_ino == other.value_.st_ino;
  }

  [[nodiscard]] std::filesystem::file_type file_type() const noexcept {
    if (is_file())
      return std::filesystem::file_type::regular;
    if (is_dir())
      return std::filesystem::file_type::directory;
    if (is_symlink())
      return std::filesystem::file_type::symlink;
    if (S_ISFIFO(value_.st_mode))
      return std::filesystem::file_type::fifo;
    if (S_ISSOCK(value_.st_mode))
      return std::filesystem::file_type::socket;
    if (S_ISBLK(value_.st_mode))
      return std::filesystem::file_type::block;
    if (S_ISCHR(value_.st_mode))
      return std::filesystem::file_type::character;
    return std::filesystem::file_type::unknown;
  }

  [[nodiscard]] std::chrono::system_clock::time_point modified() const noexcept {
#if defined(__APPLE__)
    return time(value_.st_mtimespec);
#else
    return time(value_.st_mtim);
#endif
  }

  [[nodiscard]] std::chrono::system_clock::time_point accessed() const noexcept {
#if defined(__APPLE__)
    return time(value_.st_atimespec);
#else
    return time(value_.st_atim);
#endif
  }

 private:
  static std::chrono::system_clock::time_point time(timespec value) noexcept {
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds{value.tv_sec} + std::chrono::nanoseconds{value.tv_nsec})};
  }

  struct stat value_;
};
}  // namespace faio::fs

#endif  // _WIN32
