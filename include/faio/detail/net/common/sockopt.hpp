#ifndef FAIO_DETAIL_NET_COMMON_SOCKOPT_HPP
#define FAIO_DETAIL_NET_COMMON_SOCKOPT_HPP
#include "faio/detail/common/error.hpp"
#include <chrono>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
namespace faio::net::detail {
[[nodiscard]]
static inline auto set_sock_opt(int fd, int level, int optname,
                                const void *optval, socklen_t optlen) noexcept
    -> expected<void> {
  if (::setsockopt(fd, level, optname, optval, optlen) == -1) [[unlikely]] {
    return std::unexpected{make_error(errno)};
  }
  return {};
}

[[nodiscard]]
static inline auto get_sock_opt(int fd, int level, int optname, void *optval,
                                socklen_t optlen) noexcept -> expected<void> {
  if (auto ret = ::getsockopt(fd, level, optname, optval, &optlen); ret == -1)
      [[unlikely]] {
    return std::unexpected{make_error(errno)};
  }
  return {};
}

template <class T> struct ImplNodelay {
  [[nodiscard]]
  auto set_nodelay(bool on) noexcept {
    int optval{on ? 1 : 0};
    return set_sock_opt(static_cast<T *>(this)->fd(), SOL_TCP, TCP_NODELAY,
                        &optval, sizeof(optval));
  }

  [[nodiscard]]
  auto nodelay() const noexcept -> expected<bool> {
    int optval{0};
    if (auto ret = get_sock_opt(static_cast<const T *>(this)->fd(), SOL_TCP,
                                TCP_NODELAY, &optval, sizeof(optval));
        ret) [[unlikely]] {
      return optval != 0;
    } else {
      return std::unexpected{ret.error()};
    }
  }
};

template <class T> struct ImplPasscred {
  [[nodiscard]]
  auto set_passcred(bool on) noexcept {
    int optval{on ? 1 : 0};
    return set_sock_opt(static_cast<T *>(this)->fd(), SOL_SOCKET, SO_PASSCRED,
                        &optval, sizeof(optval));
  }

  [[nodiscard]]
  auto passcred() const noexcept -> expected<bool> {
    int optval{0};
    if (auto ret = get_sock_opt(static_cast<const T *>(this)->fd(), SOL_SOCKET,
                                SO_PASSCRED, &optval, sizeof(optval));
        ret) [[likely]] {
      return optval != 0;
    } else {
      return std::unexpected{ret.error()};
    }
  }
};

template <class T> struct ImplRecvBufSize {
  [[nodiscard]]
  auto set_recv_buffer_size(int size) noexcept {
    return set_sock_opt(static_cast<T *>(this)->fd(), SOL_SOCKET, SO_RCVBUF,
                        &size, sizeof(size));
  }

  [[nodiscard]]
  auto recv_buffer_size() const noexcept -> expected<std::size_t> {
    auto size{0};
    if (auto ret = get_sock_opt(static_cast<const T *>(this)->fd(), SOL_SOCKET,
                                SO_RCVBUF, &size, sizeof(size));
        ret) [[likely]] {
      return static_cast<std::size_t>(size);
    } else {
      return std::unexpected{ret.error()};
    }
  }
};
template <class T> struct ImplSendBufSize {
  [[nodiscard]]
  auto set_send_buffer_size(int size) noexcept {
    return set_sock_opt(static_cast<T *>(this)->fd(), SOL_SOCKET, SO_SNDBUF,
                        &size, sizeof(size));
  }

  [[nodiscard]]
  auto send_buffer_size() const noexcept -> expected<std::size_t> {
    auto size{0};
    if (auto ret = get_sock_opt(static_cast<const T *>(this)->fd(), SOL_SOCKET,
                                SO_SNDBUF, &size, sizeof(size));
        ret) [[likely]] {
      return static_cast<std::size_t>(size);
    } else {
      return std::unexpected{ret.error()};
    }
  }
};

template <class T> struct ImplKeepalive {
  [[nodiscard]]
  auto set_keepalive(bool on) noexcept {
    auto optval{on ? 1 : 0};
    return set_sock_opt(static_cast<T *>(this)->fd(), SOL_SOCKET, SO_KEEPALIVE,
                        &optval, sizeof(optval));
  }

  [[nodiscard]]
  auto keepalive() const noexcept -> expected<bool> {
    auto optval{0};
    if (auto ret = get_sock_opt(static_cast<const T *>(this)->fd(), SOL_SOCKET,
                                SO_KEEPALIVE, &optval, sizeof(optval));
        ret) [[likely]] {
      return optval != 0;
    } else {
      return std::unexpected{ret.error()};
    }
  }
};

template <class T> struct ImplLinger {
  [[nodiscard]]
  auto set_linger(std::optional<std::chrono::seconds> duration) noexcept
      -> expected<void> {
    struct linger lin{
        .l_onoff{0},
        .l_linger{0},
    };
    if (duration.has_value()) {
      lin.l_onoff = 1;
      lin.l_linger = duration.value().count();
    }
    return set_sock_opt(static_cast<T *>(this)->fd(), SOL_SOCKET, SO_LINGER,
                        &lin, sizeof(lin));
  }

  [[nodiscard]]
  auto linger() const noexcept
      -> expected<std::optional<std::chrono::seconds>> {
    struct linger lin;
    if (auto ret = get_sock_opt(static_cast<const T *>(this)->fd(), SOL_SOCKET,
                                SO_LINGER, &lin, sizeof(lin));
        ret) [[likely]] {
      if (lin.l_onoff == 0) {
        return std::nullopt;
      } else {
        return std::chrono::seconds(lin.l_linger);
      }
    } else {
      return std::unexpected{ret.error()};
    }
  }
};

template <class T> struct ImplBoradcast {
  [[nodiscard]]
  auto set_broadcast(bool on) noexcept -> expected<void> {
    auto optval{on ? 1 : 0};
    return set_sock_opt(static_cast<T *>(this)->fd(), SOL_SOCKET, SO_BROADCAST,
                        &optval, sizeof(optval));
  }

  [[nodiscard]]
  auto broadcast() const noexcept -> expected<bool> {
    auto optval{0};
    if (auto ret = get_sock_opt(static_cast<const T *>(this)->fd(), SOL_SOCKET,
                                SO_BROADCAST, &optval, sizeof(optval));
        ret) [[likely]] {
      return optval != 0;
    } else {
      return std::unexpected{ret.error()};
    }
  }
};

template <class T> struct ImplTTL {
  [[nodiscard]]
  auto set_ttl(uint32_t ttl) noexcept -> expected<void> {
    return set_sock_opt(static_cast<T *>(this)->fd(), IPPROTO_IP, IP_TTL, &ttl,
                        sizeof(ttl));
  }

  [[nodiscard]]
  auto ttl() const noexcept -> expected<uint32_t> {
    uint32_t optval{0};
    if (auto ret = get_sock_opt(static_cast<const T *>(this)->fd(), IPPROTO_IP,
                                IP_TTL, &optval, sizeof(optval));
        ret) [[likely]] {
      return optval;
    } else {
      return std::unexpected{ret.error()};
    }
  }
};

template <class T> struct ImplReuseAddr {
  [[nodiscard]]
  auto set_reuseaddr(bool on) noexcept -> expected<void> {
    auto optval{on ? 1 : 0};
    return set_sock_opt(static_cast<T *>(this)->fd(), SOL_SOCKET, SO_REUSEADDR,
                        &optval, sizeof(optval));
  }

  [[nodiscard]]
  auto reuseaddr() const noexcept -> expected<bool> {
    auto optval{0};
    if (auto ret = get_sock_opt(static_cast<const T *>(this)->fd(), SOL_SOCKET,
                                SO_REUSEADDR, &optval, sizeof(optval));
        ret) [[likely]] {
      return optval != 0;
    } else {
      return std::unexpected{ret.error()};
    }
  }
};

template <class T> struct ImplReusePort {
  [[nodiscard]]
  auto set_reuseport(bool on) noexcept -> expected<void> {
    auto optval{on ? 1 : 0};
    return set_sock_opt(static_cast<T *>(this)->fd(), SOL_SOCKET, SO_REUSEPORT,
                        &optval, sizeof(optval));
  }

  [[nodiscard]]
  auto reuseport() const noexcept -> expected<bool> {
    auto optval{0};
    if (auto ret = get_sock_opt(static_cast<const T *>(this)->fd(), SOL_SOCKET,
                                SO_REUSEPORT, &optval, sizeof(optval));
        ret) [[likely]] {
      return optval != 0;
    } else {
      return std::unexpected{ret.error()};
    }
  }
};

template <class T> struct ImplMark {
  [[nodiscard]]
  auto set_mark(uint32_t mark) noexcept {
    return set_sock_opt(static_cast<T *>(this)->fd(), SOL_SOCKET, SO_MARK,
                        &mark, sizeof(mark));
  }
};
} // namespace faio::net::detail

#endif // FAIO_DETAIL_NET_COMMON_SOCKOPT_HPP