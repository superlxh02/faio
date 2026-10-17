#pragma once
/**
 * @file faio/detail/fs/platform/windows/metadata.hpp
 * @brief Windows 文件属性的拥有型、64 位身份与时间快照。
 */
#include "faio/detail/fs/platform/windows/native.hpp"
#include <array>
#include <cstring>

namespace faio::fs {
using path = std::filesystem::path;

/** @brief 便携权限值；Windows 以 FILE_ATTRIBUTE_READONLY 表示只读属性。
 * @details 不把 POSIX 模式位映射成 ACL；设置权限只修改只读属性。
 */
class Permissions {
 public:
  explicit Permissions(std::filesystem::perms bits = std::filesystem::perms::unknown) noexcept
      : bits_(bits) {}

  std::filesystem::perms bits() const noexcept { return bits_; }

  bool readonly() const noexcept { return (bits_ & write_bits) == std::filesystem::perms::none; }

  void set_readonly(bool enabled) noexcept {
    if (enabled)
      bits_ &= ~write_bits;
    else
      bits_ |= std::filesystem::perms::owner_write;
  }

 private:
  static constexpr auto write_bits = std::filesystem::perms::owner_write
                                     | std::filesystem::perms::group_write
                                     | std::filesystem::perms::others_write;  ///< 用于判断可写属性的文件权限位集合。
  std::filesystem::perms bits_;  ///< 保存的文件权限位，按平台规则映射到原生接口。
};

/** @brief 原生快照；保存完整文件长度、卷序列号和 64 位文件标识。 */
class Metadata {
 public:
  explicit Metadata(BY_HANDLE_FILE_INFORMATION value, DWORD tag = 0) noexcept
      : value_(value), tag_(tag), volume_(value.dwVolumeSerialNumber) {
    const std::uint64_t index =
        (static_cast<std::uint64_t>(value.nFileIndexHigh) << 32) | value.nFileIndexLow;
    std::memcpy(identity_.data(), &index, sizeof(index));
  }

  /** @brief FILE_ID_INFO 保留 ReFS 等文件系统的完整 128 位标识。 */
  Metadata(BY_HANDLE_FILE_INFORMATION value, DWORD tag, const FILE_ID_INFO& identity) noexcept
      : value_(value), tag_(tag), volume_(identity.VolumeSerialNumber), identity_full_(true) {
    std::memcpy(identity_.data(), identity.FileId.Identifier, identity_.size());
  }

  std::uint64_t len() const noexcept {
    return (static_cast<std::uint64_t>(value_.nFileSizeHigh) << 32) | value_.nFileSizeLow;
  }

  bool is_symlink() const noexcept {
    return (value_.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
           && (tag_ == IO_REPARSE_TAG_SYMLINK || tag_ == IO_REPARSE_TAG_MOUNT_POINT);
  }

  bool is_dir() const noexcept {
    return !is_symlink() && (value_.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  }

  bool is_file() const noexcept {
    return !is_symlink() && !(value_.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  }

  Permissions permissions() const noexcept {
    auto bits = std::filesystem::perms::owner_read | std::filesystem::perms::group_read
                | std::filesystem::perms::others_read;
    if (!(value_.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
      bits |= std::filesystem::perms::owner_write | std::filesystem::perms::group_write
              | std::filesystem::perms::others_write;
    return Permissions{bits};
  }

  bool same_file(const Metadata& other) const noexcept {
    if (identity_full_ && other.identity_full_)
      return volume_ == other.volume_ && identity_ == other.identity_;
    /// @brief 与旧格式原生快照互操作；FileIdInfo 不支持时采用 Win32 64 位身份契约。
    return value_.dwVolumeSerialNumber == other.value_.dwVolumeSerialNumber
           && value_.nFileIndexHigh == other.value_.nFileIndexHigh
           && value_.nFileIndexLow == other.value_.nFileIndexLow;
  }

  std::filesystem::file_type file_type() const noexcept {
    return is_symlink() ? std::filesystem::file_type::symlink
           : is_dir()   ? std::filesystem::file_type::directory
                        : std::filesystem::file_type::regular;
  }

  std::chrono::system_clock::time_point modified() const noexcept {
    return time(value_.ftLastWriteTime);
  }

  std::chrono::system_clock::time_point accessed() const noexcept {
    return time(value_.ftLastAccessTime);
  }

 private:
  static std::chrono::system_clock::time_point time(FILETIME value) noexcept {
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    /// @brief 1601 → 1970，单位 100 ns。
    constexpr std::uint64_t epoch = 116444736000000000ULL;
    /// @brief 先保持 100 ns 单位，避免中间纳秒乘法对极端 FILETIME 溢出。
    const std::int64_t unix_ticks = ticks >= epoch
                                        ? static_cast<std::int64_t>(std::min(
                                              ticks - epoch, static_cast<std::uint64_t>(INT64_MAX)))
                                        : -static_cast<std::int64_t>(epoch - ticks);
    using ticks_duration = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
    using target = std::chrono::system_clock::duration;
    const long double count = std::chrono::duration<long double, typename target::period>{
        ticks_duration{
            unix_ticks}}.count();
    /// @brief MSVC 的 long double 等同 double；INT64_MAX 舍入后可能变为 2^63。
    /// 在边界直接返回整数极值，避免浮点 → int64_t 的越界转换。
    if (count >= static_cast<long double>(target::max().count()))
      return std::chrono::system_clock::time_point::max();
    if (count <= static_cast<long double>(target::min().count()))
      return std::chrono::system_clock::time_point::min();
    return std::chrono::system_clock::time_point{target{static_cast<target::rep>(count)}};
  }

  BY_HANDLE_FILE_INFORMATION value_;  ///< 原生元数据快照，访问器据此返回文件属性。
  DWORD tag_{};  ///< Windows 重解析点标签，用于区分链接与其他重解析对象。
  std::uint64_t volume_{};  ///< Windows 文件所在卷的身份编号。
  std::array<std::byte, 16> identity_{};  ///< 完整文件身份字节。
  bool identity_full_{};  ///< 是否已经取得完整文件身份，决定使用哪种比较路径。
};

namespace detail {
/** @brief 从已拥有句柄查询属性，重解析类型在 no-follow 句柄上保持原样。 */
inline expected<Metadata> metadata_handle(HANDLE handle) noexcept {
  BY_HANDLE_FILE_INFORMATION value{};
  if (!::GetFileInformationByHandle(handle, &value))
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  DWORD tag = 0;
  if (value.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!::GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes, sizeof(attributes)))
      return std::unexpected{io::windows::make_windows_error(::GetLastError())};
    tag = attributes.ReparseTag;
  }
  FILE_ID_INFO identity{};
  if (::GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)))
    return Metadata{value, tag, identity};
  const DWORD error = ::GetLastError();
  if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED
      && error != ERROR_INVALID_FUNCTION)
    return std::unexpected{io::windows::make_windows_error(error)};
  /// @brief 不支持 128 位标识的文件系统仍保留旧 64 位身份。
  return Metadata{value, tag};
}

/** @brief 设置句柄只读位，保留其他属性和所有时间戳。 */
inline expected<void> set_handle_permissions(HANDLE handle, Permissions permissions) noexcept {
  FILE_BASIC_INFO attributes{};
  if (!::GetFileInformationByHandleEx(handle, FileBasicInfo, &attributes, sizeof(attributes)))
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  if (permissions.readonly())
    attributes.FileAttributes |= FILE_ATTRIBUTE_READONLY;
  else
    attributes.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
  if (!attributes.FileAttributes)
    attributes.FileAttributes = FILE_ATTRIBUTE_NORMAL;
  if (!::SetFileInformationByHandle(handle, FileBasicInfo, &attributes, sizeof(attributes)))
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  return {};
}
}
}
