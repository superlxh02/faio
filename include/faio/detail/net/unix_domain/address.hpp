/**
 * @file faio/detail/net/unix_domain/address.hpp
 * @brief socket 地址接口。
 */
#ifndef FAIO_DETAIL_NET_UNIX_DOMAIN_ADDRESS_HPP
#define FAIO_DETAIL_NET_UNIX_DOMAIN_ADDRESS_HPP
#include "faio/detail/common/error.hpp"
#include <string_view>
#include "faio/detail/net/platform/select.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>

namespace faio::net::unix::detail {
/**
 *  @brief 拥有型 Unix 地址，区分 pathname、unnamed 与 Linux abstract
 * namespace。
 */
class UnixAddr {
 public:
  /** @brief 构造未命名的 Unix 地址。 */
  UnixAddr() noexcept { address_.sun_family = AF_UNIX; }

  /**  @brief 创建 pathname 地址；拒绝内嵌 NUL 和超长路径，不截断用户输入。 */
  [[nodiscard]] static auto pathname(std::string_view path) -> expected<UnixAddr> {
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)
        || path.find('\0') != std::string_view::npos)
      return std::unexpected{make_error(EINVAL)};
    UnixAddr address;
    std::memcpy(address.address_.sun_path, path.data(), path.size());
    address.length_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    address.update_length();
    return address;
  }

  /** @brief 解析地址文本，非法格式返回错误。 */
  [[nodiscard]] static auto parse(std::string_view path) -> expected<UnixAddr> {
    return pathname(path);
  }

  /**
   *  @brief Linux abstract 地址允许名字内嵌 NUL；其他 Unix 平台明确返回
   * ENOTSUP。
   */
  [[nodiscard]] static auto abstract(std::string_view name) -> expected<UnixAddr> {
    UnixAddr address;
    auto length = ::faio::net::detail::make_abstract_address(address.address_, name);
    if (!length)
      return std::unexpected{length.error()};
    address.length_ = *length;
    return address;
  }

  /** @brief 返回地址或 socket 所属的原生地址族。 */
  [[nodiscard]] auto family() const noexcept -> int { return AF_UNIX; }

  /** @brief 借用原生 socket 地址存储，不转移其生命周期管理责任。 */
  [[nodiscard]] auto sockaddr() noexcept -> struct sockaddr* {
    return reinterpret_cast<struct sockaddr*>(&address_);
  }

  /** @brief 借用原生 socket 地址存储，不转移其生命周期管理责任。 */
  [[nodiscard]] auto sockaddr() const noexcept -> const struct sockaddr* {
    return reinterpret_cast<const struct sockaddr*>(&address_);
  }

  /** @brief 返回传给原生 socket 接口的有效地址字节数。 */
  [[nodiscard]] auto length() const noexcept -> socklen_t { return length_; }

  /** @brief 返回原生 socket 地址存储的字节容量。 */
  [[nodiscard]] static constexpr auto capacity() noexcept -> socklen_t {
    return sizeof(sockaddr_un);
  }

  /**  @brief 接收查询结果后记录 OS 返回长度，限制在实际存储范围。 */
  void set_length(socklen_t length) noexcept {
    length_ =
        std::clamp(length, static_cast<socklen_t>(offsetof(sockaddr_un, sun_path)), capacity());
  }

  /** @brief 判断 Unix 地址是否不包含名称。 */
  [[nodiscard]] auto is_unnamed() const noexcept -> bool {
    return length_ <= offsetof(sockaddr_un, sun_path);
  }

  /** @brief 判断 Unix 地址是否位于 Linux 抽象命名空间。 */
  [[nodiscard]] auto is_abstract() const noexcept -> bool {
    return !is_unnamed() && address_.sun_path[0] == '\0';
  }

  /** @brief 返回对象保存的文件系统路径。 */
  [[nodiscard]] auto path() const noexcept -> std::string_view {
    if (is_unnamed() || is_abstract())
      return {};
    const auto bytes = length_ - offsetof(sockaddr_un, sun_path);
    return {address_.sun_path, ::strnlen(address_.sun_path, bytes)};
  }

  /** @brief 借用 Unix 抽象地址的名称字节。 */
  [[nodiscard]] auto abstract_name() const noexcept -> std::string_view {
    if (!is_abstract())
      return {};
    return {address_.sun_path + 1, length_ - offsetof(sockaddr_un, sun_path) - 1};
  }

  /** @brief 将地址格式化为可读文本。 */
  [[nodiscard]] auto to_string() const -> std::string {
    if (is_unnamed())
      return "(unnamed)";
    if (is_abstract())
      return "@" + std::string{abstract_name()};
    return std::string{path()};
  }

  /** @brief 比较两个地址值是否相等。 */
  friend auto operator==(const UnixAddr& left, const UnixAddr& right) noexcept -> bool {
    return left.length_ == right.length_
           && std::memcmp(&left.address_, &right.address_, left.length_) == 0;
  }

 private:
  /** @brief 根据地址族更新原生地址的有效长度。 */
  void update_length() noexcept {
    ::faio::net::detail::set_native_address_length(address_, length_);
  }

  sockaddr_un address_{}; /**< 拥有的本地或对端 socket 地址，提交期间不借用临时对象。 */
  socklen_t length_{
      offsetof(sockaddr_un, sun_path)}; /**< 原生 socket 地址的实际长度，含平台要求的地址头部。 */
};
}  // namespace faio::net::unix::detail
#endif
