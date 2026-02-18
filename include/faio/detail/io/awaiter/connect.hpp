#ifndef FAIO_DETAIL_IO_AWAITER_CONNECT_HPP
#define FAIO_DETAIL_IO_AWAITER_CONNECT_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class Connect : public IORegistrantAwaiter<Connect> {
private:
  using Base = IORegistrantAwaiter<Connect>;

public:
  Connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
      : Base{io_uring_prep_connect, fd, addr, addrlen} {}

  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result >= 0) [[likely]] {
      return {};
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_CONNECT_HPP