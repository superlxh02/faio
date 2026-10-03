#ifndef FAIO_DETAIL_NET_COMMON_ADDR_UTIL_HPP
#define FAIO_DETAIL_NET_COMMON_ADDR_UTIL_HPP
#include "faio/detail/net/common/platform.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/io.hpp"

namespace faio::net::detail {
/** @brief 查询对端；长度使用地址存储容量，兼容 IPv6 与 Unix pathname。 */
template <class T, class Addr>
struct ImplPeerAddr {
  [[nodiscard]] auto peer_addr() const noexcept -> expected<Addr> {
    const auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::Interest::none, [&]() -> expected<Addr> {
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

/** @brief 查询本地地址；bind(端口0) 后可获得内核分配的实际端口。 */
template <class T, class Addr>
struct ImplLocalAddr {
  [[nodiscard]] auto local_addr() const noexcept -> expected<Addr> {
    const auto* object = static_cast<const T*>(this);
    return io::detail::with_resource(
        object->resource(), io::Interest::none, [&]() -> expected<Addr> {
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
