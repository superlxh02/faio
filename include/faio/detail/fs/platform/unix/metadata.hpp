/**
 * @file faio/detail/fs/platform/unix/metadata.hpp
 * @brief 文件与目录异步接口：文件元数据。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
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
  std::filesystem::perms bits_;  ///< 保存的文件权限位，按平台规则映射到原生接口。
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
  explicit Metadata(const struct statx& value) noexcept : value_(detail::stat_from_statx(value)) {}
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
    return time(detail::modified_time(value_));
  }

  [[nodiscard]] std::chrono::system_clock::time_point accessed() const noexcept {
    return time(detail::accessed_time(value_));
  }

 private:
  static std::chrono::system_clock::time_point time(timespec value) noexcept {
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds{value.tv_sec} + std::chrono::nanoseconds{value.tv_nsec})};
  }

  struct stat value_;  ///< 原生元数据快照，访问器据此返回文件属性。
};
}
