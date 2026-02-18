#ifndef FAIO_DETAIL_IO_AWAITER_RECV_HPP
#define FAIO_DETAIL_IO_AWAITER_RECV_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class Recv : public IORegistrantAwaiter<Recv> {
private:
  using Base = IORegistrantAwaiter<Recv>;

public:
  Recv(int sockfd, void *buf, size_t len, int flags)
      : Base{io_uring_prep_recv, sockfd, buf, len, flags} {}

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result >= 0) [[likely]] {
      return static_cast<std::size_t>(this->_user_data.result);
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_RECV_HPP