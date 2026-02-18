#ifndef FAIO_DETAIL_NET_COMMON_DATAGRAM_SEND_HPP
#define FAIO_DETAIL_NET_COMMON_DATAGRAM_SEND_HPP

#include "faio/detail/common/error.hpp"
#include "faio/detail/io/awaiter/send.hpp"
#include "faio/detail/io/awaiter/sendto.hpp"
namespace faio::net::detail {
template <class T, class Addr> struct ImplSend {
  auto send(std::span<const char> buf) noexcept {
    return io::detail::Send{static_cast<T *>(this)->fd(), buf.data(),
                            buf.size_bytes(), MSG_NOSIGNAL};
  }

  auto send_to(std::span<const char> buf, const Addr &addr) noexcept {
    return io::detail::SendTo{static_cast<T *>(this)->fd(),
                              buf.data(),
                              buf.size_bytes(),
                              MSG_NOSIGNAL,
                              addr.sockaddr(),
                              addr.length()};
  }
};
} // namespace faio::net::detail
#endif // FAIO_DETAIL_NET_COMMON_DATAGRAM_SEND_HPP