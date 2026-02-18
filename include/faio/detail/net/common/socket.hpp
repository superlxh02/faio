#ifndef FAIO_DETAIL_NET_COMMON_SOCKET_HPP
#define FAIO_DETAIL_NET_COMMON_SOCKET_HPP

#include "faio/detail/common/concepts.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/awaiter/shutdown.hpp"
#include "faio/detail/io/io.hpp"
#include <sys/socket.h>

namespace faio::net::detail {
class Socket : public io::detail::FileDescriptor {
public:
  explicit Socket(const int fd) : FileDescriptor{fd} {}

public:
  template <typename Addr>
    requires is_socket_address<Addr>
  [[nodiscard]]
  auto bind(const Addr &addr) -> expected<void> {
    if (::bind(_fd, addr.sockaddr(), addr.length()) != 0) [[unlikely]] {
      return std::unexpected{make_error(errno)};
    }
    return {};
  }

  [[nodiscard]]
  auto listen(int maxn = SOMAXCONN) -> expected<void> {
    if (::listen(_fd, maxn) != 0) [[unlikely]] {
      return std::unexpected{make_error(errno)};
    }
    return {};
  }

  auto shutdown(io::ShutdownBehavior how) noexcept {
    return io::detail::Shutdown{fd(), how};
  }

public:
  template <typename T = Socket>
  [[nodiscard]]
  static auto create(const int domain, const int type, const int protocol)
      -> expected<T> {
    auto fd = ::socket(domain, type, protocol);
    if (fd < 0) [[unlikely]] {
      return std::unexpected{make_error(errno)};
    }
    return T{Socket{fd}};
  }
};
} // namespace faio::net::detail
#endif // FAIO_DETAIL_NET_COMMON_SOCKET_HPP