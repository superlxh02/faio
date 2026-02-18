#ifndef FAIO_DETAIL_IO_AWAITER_SOCKET_HPP
#define FAIO_DETAIL_IO_AWAITER_SOCKET_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class Socket : public detail::IORegistrantAwaiter<Socket> {
private:
  using Base = detail::IORegistrantAwaiter<Socket>;

public:
  Socket(int domain, int type, int protocol, unsigned int flags)
      : Base{io_uring_prep_socket, domain, type, protocol, flags} {}

  auto await_resume() const noexcept -> expected<int> {
    if (this->_user_data.result >= 0) [[likely]] {
      return this->_user_data.result;
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_SOCKET_HPP