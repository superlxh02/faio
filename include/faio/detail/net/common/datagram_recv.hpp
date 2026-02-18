#ifndef FAIO_DETAIL_NET_COMMON_DATAGRAM_RECV_HPP
#define FAIO_DETAIL_NET_COMMON_DATAGRAM_RECV_HPP

#include "faio/detail/io/awaiter/recv.hpp"
#include "faio/detail/io/base/io_registrant.hpp"
namespace faio::net::detail {
template <class T, class Addr> struct ImplRecv {
  auto recv(std::span<char> buf, int flags = 0) const noexcept {
    return io::detail::Recv{static_cast<const T *>(this)->fd(), buf.data(),
                            buf.size_bytes(), flags};
  }

  auto peek(std::span<char> buf) const noexcept {
    return this->recv(buf, MSG_PEEK);
  }

  auto recv_from(std::span<char> buf, unsigned flags = 0) const noexcept {
    class RecvFrom : public io::detail::IORegistrantAwaiter<RecvFrom> {
    private:
      using Base = io::detail::IORegistrantAwaiter<RecvFrom>;

    public:
      RecvFrom(int fd, std::span<char> buf, unsigned flags)
          : Base{io_uring_prep_recvmsg, fd, &_msg, flags},
            _iovecs{.iov_base = buf.data(), .iov_len = buf.size_bytes()},
            _msg{.msg_name = &_addr,
                 .msg_namelen = sizeof(_addr),
                 .msg_iov = &_iovecs,
                 .msg_iovlen = 1,
                 .msg_control = nullptr,
                 .msg_controllen = 0,
                 .msg_flags = static_cast<int>(flags)} {};

      auto await_resume() const noexcept
          -> expected<std::pair<std::size_t, Addr>> {
        if (this->_user_data.result >= 0) [[likely]] {
          return std::make_pair(static_cast<std::size_t>(this->_user_data.result),
                                _addr);
        } else {
          return std::unexpected{make_error(-this->_user_data.result)};
        }
      }

    private:
      struct iovec _iovecs;
      Addr _addr{};
      struct msghdr _msg{};
    };
    return RecvFrom{static_cast<const T *>(this)->fd(), buf, flags};
  }

  auto peek_from(std::span<char> buf) const noexcept {
    return this->recv_from(buf, MSG_PEEK);
  }
};
} // namespace faio::net::detail
#endif // FAIO_DETAIL_NET_COMMON_DATAGRAM_RECV_HPP