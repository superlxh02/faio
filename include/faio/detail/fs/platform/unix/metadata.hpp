/**
 * @file faio/detail/fs/platform/unix/metadata.hpp
 * @brief 文件元数据。
 */
#pragma once
#include "faio/detail/common/error.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <sys/stat.h>
#include "faio/detail/fs/platform/unix/stat_time.hpp"
#if defined(__linux__)
#include "faio/detail/fs/platform/linux/statx.hpp"
#endif

namespace faio::fs {
using path = std::filesystem::path;

/**  @brief 保存 POSIX 权限位的可移植文件权限值。 */
class Permissions {
 public:
  /** @brief 保存平台无关的文件权限值。 */
  explicit Permissions(std::filesystem::perms bits = std::filesystem::perms::unknown) noexcept
      : bits_(bits) {}

  /** @brief 返回保存的文件权限位。 */
  [[nodiscard]] std::filesystem::perms bits() const noexcept { return bits_; }

  /** @brief 判断权限是否禁止写入。 */
  [[nodiscard]] bool readonly() const noexcept {
    return (bits_
            & (std::filesystem::perms::owner_write | std::filesystem::perms::group_write
               | std::filesystem::perms::others_write))
           == std::filesystem::perms::none;
  }

  /** @brief 设置只读状态，按平台权限模型转换。 */
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
  std::filesystem::perms bits_; /**< 保存的文件权限位，按平台规则映射到原生接口。 */
};

/**  @brief 持有一次原生 STATX 或 stat/lstat/fstat 查询得到的属性快照。 */
class Metadata {
 public:
  /** @brief 保存独立持有的原生文件属性快照。 */
  explicit Metadata(struct stat value) noexcept : value_(value) {}
#if defined(__linux__)
  /**
   *  @brief 将原生 STATX_BASIC_STATS 结果转换为统一属性，无需再次调用 stat。
   * @details 保留设备/inode 身份、权限、长度与纳秒时间；转换只读取 CQE 已经完成的
   *          输出内存。设备号使用系统 makedev 编码，与 fstat 的 same_file 兼容。
   */
  explicit Metadata(const struct statx& value) noexcept : value_(detail::stat_from_statx(value)) {}
#endif
  /** @brief 返回文件属性记录的字节长度。 */
  [[nodiscard]] std::uint64_t len() const noexcept {
    return static_cast<std::uint64_t>(value_.st_size);
  }

  /** @brief 判断元数据是否表示普通文件。 */
  [[nodiscard]] bool is_file() const noexcept { return S_ISREG(value_.st_mode); }

  /** @brief 判断元数据是否表示目录。 */
  [[nodiscard]] bool is_dir() const noexcept { return S_ISDIR(value_.st_mode); }

  /** @brief 判断元数据是否表示符号链接。 */
  [[nodiscard]] bool is_symlink() const noexcept { return S_ISLNK(value_.st_mode); }

  /** @brief 返回元数据快照保存的文件权限。 */
  [[nodiscard]] Permissions permissions() const noexcept {
    return Permissions{static_cast<std::filesystem::perms>(value_.st_mode & 07777)};
  }

  /**
   *  @brief
   * 检查两份 POSIX 属性快照是否指向同一设备上的同一 inode，防止复制到自身时截断文件。
   */
  [[nodiscard]] bool same_file(const Metadata& other) const noexcept {
    return value_.st_dev == other.value_.st_dev && value_.st_ino == other.value_.st_ino;
  }

  /** @brief 返回文件或目录项的类型。 */
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

  /** @brief 返回文件修改时间。 */
  [[nodiscard]] std::chrono::system_clock::time_point modified() const noexcept {
    return time(detail::modified_time(value_));
  }

  /** @brief 返回文件访问时间。 */
  [[nodiscard]] std::chrono::system_clock::time_point accessed() const noexcept {
    return time(detail::accessed_time(value_));
  }

 private:
  /** @brief 将原生时间值转换为 system_clock 时间点。 */
  static std::chrono::system_clock::time_point time(timespec value) noexcept {
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds{value.tv_sec} + std::chrono::nanoseconds{value.tv_nsec})};
  }

  struct stat value_; /**< 原生元数据快照，访问器据此返回文件属性。 */
};
}  // namespace faio::fs
