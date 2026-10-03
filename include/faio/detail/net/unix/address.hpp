#ifndef FAIO_DETAIL_NET_UNIX_ADDRESS_HPP
#define FAIO_DETAIL_NET_UNIX_ADDRESS_HPP
#include "faio/detail/common/error.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
// GCC 的 GNU 模式预定义 unix=1，与标准命名空间名冲突，公共头消除此历史宏。
#ifdef unix
#undef unix
#endif

namespace faio::net::unix::detail {
/** @brief 拥有型 Unix 地址，区分 pathname、unnamed 与 Linux abstract
 * namespace。 */
class UnixAddr {
public:
  UnixAddr() noexcept { address_.sun_family = AF_UNIX; }
  /** @brief 创建 pathname 地址；拒绝内嵌 NUL 和超长路径，不截断用户输入。 */
  [[nodiscard]] static auto pathname(std::string_view path)
      -> expected<UnixAddr> {
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path) ||
        path.find('\0') != std::string_view::npos)
      return std::unexpected{make_error(EINVAL)};
    UnixAddr address;
    std::memcpy(address.address_.sun_path, path.data(), path.size());
    address.length_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                             path.size() + 1);
    address.update_length();
    return address;
  }
  [[nodiscard]] static auto parse(std::string_view path) -> expected<UnixAddr> {
    return pathname(path);
  }
  /** @brief Linux abstract 地址允许名字内嵌 NUL；其他 Unix 平台明确返回
   * ENOTSUP。 */
  [[nodiscard]] static auto abstract(std::string_view name)
      -> expected<UnixAddr> {
#if defined(__linux__)
    if (name.size() > sizeof(sockaddr_un::sun_path) - 1)
      return std::unexpected{make_error(ENAMETOOLONG)};
    UnixAddr address;
    std::memcpy(address.address_.sun_path + 1, name.data(), name.size());
    address.length_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                             1 + name.size());
    return address;
#else
    (void)name;
    return std::unexpected{make_error(ENOTSUP)};
#endif
  }
  [[nodiscard]] auto family() const noexcept -> int { return AF_UNIX; }
  [[nodiscard]] auto sockaddr() noexcept -> struct sockaddr * {
    return reinterpret_cast<struct sockaddr *>(&address_);
  }
  [[nodiscard]] auto sockaddr() const noexcept -> const struct sockaddr * {
    return reinterpret_cast<const struct sockaddr *>(&address_);
  }
  [[nodiscard]] auto length() const noexcept -> socklen_t { return length_; }
  [[nodiscard]] static constexpr auto capacity() noexcept -> socklen_t {
    return sizeof(sockaddr_un);
  }
  /** @brief 接收查询结果后记录 OS 返回长度，限制在实际存储范围。 */
  void set_length(socklen_t length) noexcept {
    length_ = std::clamp(
        length, static_cast<socklen_t>(offsetof(sockaddr_un, sun_path)),
        capacity());
  }
  [[nodiscard]] auto is_unnamed() const noexcept -> bool {
    return length_ <= offsetof(sockaddr_un, sun_path);
  }
  [[nodiscard]] auto is_abstract() const noexcept -> bool {
    return !is_unnamed() && address_.sun_path[0] == '\0';
  }
  [[nodiscard]] auto path() const noexcept -> std::string_view {
    if (is_unnamed() || is_abstract())
      return {};
    const auto bytes = length_ - offsetof(sockaddr_un, sun_path);
    return {address_.sun_path, ::strnlen(address_.sun_path, bytes)};
  }
  [[nodiscard]] auto abstract_name() const noexcept -> std::string_view {
    if (!is_abstract())
      return {};
    return {address_.sun_path + 1,
            length_ - offsetof(sockaddr_un, sun_path) - 1};
  }
  [[nodiscard]] auto to_string() const -> std::string {
    if (is_unnamed())
      return "(unnamed)";
    if (is_abstract())
      return "@" + std::string{abstract_name()};
    return std::string{path()};
  }
  friend auto operator==(const UnixAddr &left, const UnixAddr &right) noexcept
      -> bool {
    return left.length_ == right.length_ &&
           std::memcmp(&left.address_, &right.address_, left.length_) == 0;
  }

private:
  void update_length() noexcept {
#if defined(__APPLE__) || defined(__FreeBSD__)
    address_.sun_len = static_cast<unsigned char>(length_);
#endif
  }
  sockaddr_un address_{};
  socklen_t length_{offsetof(sockaddr_un, sun_path)};
};
} // namespace faio::net::unix::detail
#endif
