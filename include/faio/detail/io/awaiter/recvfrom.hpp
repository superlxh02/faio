#ifndef FAIO_DETAIL_IO_AWAITER_RECVFROM_HPP
#define FAIO_DETAIL_IO_AWAITER_RECVFROM_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

// TODOOODOOOOO wait io_uring_prep_recvfrom

class RecvFrom : public IORegistrantAwaiter<RecvFrom> {
private:
  using Base = IORegistrantAwaiter<RecvFrom>;

public:
  RecvFrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *addr,
           socklen_t *addrlen)
      : Base{io_uring_prep_recvmsg, sockfd, &msg_, flags},
        iovec_{.iov_base = buf, .iov_len = len},
        msg_{.msg_name = addr,
             .msg_namelen = addrlen != nullptr ? *addrlen : 0,
             .msg_iov = &iovec_,
             .msg_iovlen = len,
             .msg_control = nullptr,
             .msg_controllen = 0,
             .msg_flags = flags},
        addrlen_{addrlen} {}

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (addrlen_) {
      *addrlen_ = msg_.msg_namelen;
    }
    if (this->_user_data.result >= 0) [[likely]] {
      return static_cast<std::size_t>(this->_user_data.result);
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }

private:
  struct iovec iovec_;
  struct msghdr msg_;
  socklen_t *addrlen_;
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_RECVFROM_HPP