#ifndef FAIO_DETAIL_IO_AWAITER_SEND_HPP
#define FAIO_DETAIL_IO_AWAITER_SEND_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class Send : public IORegistrantAwaiter<Send> {
private:
  using Base = IORegistrantAwaiter<Send>;

public:
  Send(int sockfd, const void *buf, size_t len, int flags)
      : Base{io_uring_prep_send, sockfd, buf, len, flags} {
    // std::cout << "send fd is" << sockfd << std::endl;
  }

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result >= 0) [[likely]] {
      return static_cast<std::size_t>(this->_user_data.result);
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

class SendZC : public IORegistrantAwaiter<SendZC> {
private:
  using Base = IORegistrantAwaiter<SendZC>;

public:
  SendZC(int sockfd, const void *buf, size_t len, int flags, unsigned zc_flags)
      : Base{io_uring_prep_send_zc, sockfd, buf, len, flags, zc_flags} {}

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result >= 0) [[likely]] {
      return static_cast<std::size_t>(this->_user_data.result);
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_SEND_HPP
