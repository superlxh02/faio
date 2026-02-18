#ifndef FAIO_DETAIL_NET_COMMON_ADDR_UTIL_HPP
#define FAIO_DETAIL_NET_COMMON_ADDR_UTIL_HPP

#include "faio/detail/common/error.hpp"
#include <sys/socket.h>

namespace faio::net::detail {
template <class T, class Addr> struct ImplPeerAddr {
  [[nodiscard]]
  auto peer_addr() const noexcept -> expected<Addr> {
    Addr addr{};
    socklen_t len{sizeof(addr)};
    if (::getpeername(static_cast<const T *>(this)->fd(), addr.sockaddr(),
                      &len) == -1) {
      return std::unexpected{make_error(errno)};
    }
    return addr;
  }
};

template <class T, class Addr> struct ImplLocalAddr {
  [[nodiscard]]
  auto local_addr() const noexcept -> expected<Addr> {
    Addr addr{};
    socklen_t len{sizeof(addr)};
    if (::getsockname(static_cast<const T *>(this)->fd(), addr.sockaddr(),
                      &len) == -1) {
      return std::unexpected{make_error(errno)};
    }
    return addr;
  }
};
} // namespace faio::net::detail

#endif // FAIO_DETAIL_NET_COMMON_ADDR_UTIL_HPP