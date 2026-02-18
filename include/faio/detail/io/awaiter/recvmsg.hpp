#ifndef FAIO_DETAIL_IO_AWAITER_RECVMSG_HPP
#define FAIO_DETAIL_IO_AWAITER_RECVMSG_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class RecvMsg : public IORegistrantAwaiter<RecvMsg> {
private:
  using Base = IORegistrantAwaiter<RecvMsg>;

public:
  RecvMsg(int fd, struct msghdr *msg, unsigned flags)
      : Base{io_uring_prep_recvmsg, fd, msg, flags} {}

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result >= 0) [[likely]] {
      return static_cast<std::size_t>(this->_user_data.result);
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_RECVMSG_HPP