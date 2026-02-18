#ifndef FAIO_DETAIL_IO_AWAITER_SENDTO_HPP
#define FAIO_DETAIL_IO_AWAITER_SENDTO_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class SendTo : public IORegistrantAwaiter<SendTo> {
private:
  using Base = IORegistrantAwaiter<SendTo>;

public:
  SendTo(int sockfd, const void *buf, size_t len, int flags,
         const struct sockaddr *addr, socklen_t addrlen)
      : Base{io_uring_prep_sendto, sockfd, buf, len, flags, addr, addrlen} {}

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result >= 0) [[likely]] {
      return static_cast<std::size_t>(this->_user_data.result);
    } else {
      return std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_SENDTO_HPP