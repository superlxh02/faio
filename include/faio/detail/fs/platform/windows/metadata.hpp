#pragma once
/**
 * @file faio/detail/fs/platform/windows/metadata.hpp
 * @brief 独立持有的 Windows 文件属性快照，包含 64 位文件标识和时间。
 */
#include "faio/detail/fs/platform/windows/native.hpp"
#include <array>
#include <cstring>

namespace faio::fs {
using path = std::filesystem::path;

/**
 *  @brief 可移植的权限值；Windows 通过 FILE_ATTRIBUTE_READONLY 表示只读属性。
 * @details 不把 POSIX 模式位映射成 ACL；设置权限只修改只读属性。
 */
class Permissions {
 public:
  /** @brief 保存平台无关的文件权限值。 */
  explicit Permissions(std::filesystem::perms bits = std::filesystem::perms::unknown) noexcept
      : bits_(bits) {}

  /** @brief 返回保存的文件权限位。 */
  std::filesystem::perms bits() const noexcept { return bits_; }

  /** @brief 判断权限是否禁止写入。 */
  bool readonly() const noexcept { return (bits_ & write_bits) == std::filesystem::perms::none; }

  /** @brief 设置只读状态，按平台权限模型转换。 */
  void set_readonly(bool enabled) noexcept {
    if (enabled)
      bits_ &= ~write_bits;
    else
      bits_ |= std::filesystem::perms::owner_write;
  }

 private:
  static constexpr auto write_bits =
      std::filesystem::perms::owner_write | std::filesystem::perms::group_write
      | std::filesystem::perms::others_write; /**< 用于判断可写属性的文件权限位集合。 */
  std::filesystem::perms bits_;               /**< 保存的文件权限位，按平台规则映射到原生接口。 */
};

/**  @brief 原生快照；保存完整文件长度、卷序列号和 64 位文件标识。 */
class Metadata {
 public:
  /** @brief 保存独立持有的原生文件属性快照。 */
  explicit Metadata(BY_HANDLE_FILE_INFORMATION value, DWORD tag = 0) noexcept
      : value_(value), tag_(tag), volume_(value.dwVolumeSerialNumber) {
    const std::uint64_t index =
        (static_cast<std::uint64_t>(value.nFileIndexHigh) << 32) | value.nFileIndexLow;
    std::memcpy(identity_.data(), &index, sizeof(index));
  }

  /**  @brief FILE_ID_INFO 保留 ReFS 等文件系统的完整 128 位标识。 */
  Metadata(BY_HANDLE_FILE_INFORMATION value, DWORD tag, const FILE_ID_INFO& identity) noexcept
      : value_(value), tag_(tag), volume_(identity.VolumeSerialNumber), identity_full_(true) {
    std::memcpy(identity_.data(), identity.FileId.Identifier, identity_.size());
  }

  /** @brief 返回文件属性记录的字节长度。 */
  std::uint64_t len() const noexcept {
    return (static_cast<std::uint64_t>(value_.nFileSizeHigh) << 32) | value_.nFileSizeLow;
  }

  /** @brief 判断元数据是否表示符号链接。 */
  bool is_symlink() const noexcept {
    return (value_.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
           && (tag_ == IO_REPARSE_TAG_SYMLINK || tag_ == IO_REPARSE_TAG_MOUNT_POINT);
  }

  /** @brief 判断元数据是否表示目录。 */
  bool is_dir() const noexcept {
    return !is_symlink() && (value_.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  }

  /** @brief 判断元数据是否表示普通文件。 */
  bool is_file() const noexcept {
    return !is_symlink() && !(value_.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  }

  /** @brief 返回元数据快照保存的文件权限。 */
  Permissions permissions() const noexcept {
    auto bits = std::filesystem::perms::owner_read | std::filesystem::perms::group_read
                | std::filesystem::perms::others_read;
    if (!(value_.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
      bits |= std::filesystem::perms::owner_write | std::filesystem::perms::group_write
              | std::filesystem::perms::others_write;
    return Permissions{bits};
  }

  /** @brief 判断两份元数据是否标识同一个文件对象。 */
  bool same_file(const Metadata& other) const noexcept {
    if (identity_full_ && other.identity_full_)
      return volume_ == other.volume_ && identity_ == other.identity_;
    return value_.dwVolumeSerialNumber == other.value_.dwVolumeSerialNumber
           && value_.nFileIndexHigh == other.value_.nFileIndexHigh
           && value_.nFileIndexLow == other.value_.nFileIndexLow;
  }

  /** @brief 返回文件或目录项的类型。 */
  std::filesystem::file_type file_type() const noexcept {
    return is_symlink() ? std::filesystem::file_type::symlink
           : is_dir()   ? std::filesystem::file_type::directory
                        : std::filesystem::file_type::regular;
  }

  /** @brief 返回文件修改时间。 */
  std::chrono::system_clock::time_point modified() const noexcept {
    return time(value_.ftLastWriteTime);
  }

  /** @brief 返回文件访问时间。 */
  std::chrono::system_clock::time_point accessed() const noexcept {
    return time(value_.ftLastAccessTime);
  }

 private:
  /** @brief 将原生时间值转换为 system_clock 时间点。 */
  static std::chrono::system_clock::time_point time(FILETIME value) noexcept {
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    constexpr std::uint64_t epoch = 116444736000000000ULL;
    const std::int64_t unix_ticks = ticks >= epoch
                                        ? static_cast<std::int64_t>(std::min(
                                              ticks - epoch, static_cast<std::uint64_t>(INT64_MAX)))
                                        : -static_cast<std::int64_t>(epoch - ticks);
    using ticks_duration = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
    using target = std::chrono::system_clock::duration;
    const long double count = std::chrono::duration<long double, typename target::period>{
        ticks_duration{
            unix_ticks}}.count();
    if (count >= static_cast<long double>(target::max().count()))
      return std::chrono::system_clock::time_point::max();
    if (count <= static_cast<long double>(target::min().count()))
      return std::chrono::system_clock::time_point::min();
    return std::chrono::system_clock::time_point{target{static_cast<target::rep>(count)}};
  }

  BY_HANDLE_FILE_INFORMATION value_; /**< 原生元数据快照，访问器据此返回文件属性。 */
  DWORD tag_{};                      /**< Windows 重解析点标签，用于区分链接与其他重解析对象。 */
  std::uint64_t volume_{};           /**< Windows 文件所在卷的身份编号。 */
  std::array<std::byte, 16> identity_{}; /**< 完整文件身份字节。 */
  bool identity_full_{};                 /**< 是否已经取得完整文件身份，决定使用哪种比较路径。 */
};

namespace detail {
/**  @brief 通过已持有的句柄查询属性；使用不跟随链接的句柄时，保留重解析点的原始类型。 */
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
  return Metadata{value, tag};
}

/**  @brief 设置句柄只读位，保留其他属性和所有时间戳。 */
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
}  // namespace detail
}  // namespace faio::fs
