/**
 * @file faio/detail/net/base/address.hpp
 * @brief socket 地址接口。
 */
#ifndef FAIO_DETAIL_NET_BASE_ADDRESS_HPP
#define FAIO_DETAIL_NET_BASE_ADDRESS_HPP
#include "faio/detail/net/platform/select.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/core/resource_impl.hpp"

namespace faio::net::detail {
/**  @brief 查询对端；长度使用地址存储容量，兼容 IPv6 与 Unix pathname。 */
template <class T, class Addr>
struct impl_peer_addr {
  /** @brief 查询 socket 的对端地址。 */
  [[nodiscard]] auto peer_addr() const noexcept -> expected<Addr> {
    const auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::interest::NONE, [&]() -> expected<Addr> {
          Addr address{};
          socklen_t length{Addr::capacity()};
          if (::getpeername(object->fd(), address.sockaddr(), &length) < 0)
            return std::unexpected{socket_error()};
          if constexpr (requires { address.set_length(length); })
            address.set_length(length);
          return address;
        });
  }
};

/**  @brief 查询本地地址；bind(端口0) 后可获得内核分配的实际端口。 */
template <class T, class Addr>
struct impl_local_addr {
  /** @brief 查询 socket 的本地地址。 */
  [[nodiscard]] auto local_addr() const noexcept -> expected<Addr> {
    const auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::interest::NONE, [&]() -> expected<Addr> {
          Addr address{};
          socklen_t length{Addr::capacity()};
          if (::getsockname(object->fd(), address.sockaddr(), &length) < 0)
            return std::unexpected{socket_error()};
          if constexpr (requires { address.set_length(length); })
            address.set_length(length);
          return address;
        });
  }
};
}  // namespace faio::net::detail
#endif
