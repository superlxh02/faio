#ifndef FAIO_DETAIL_IO_AWAITER_SENDMSG_HPP
#define FAIO_DETAIL_IO_AWAITER_SENDMSG_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class SendMsg : public IORegistrantAwaiter<SendMsg> {
private:
  using Base = IORegistrantAwaiter<SendMsg>;

public:
  SendMsg(int fd, const struct msghdr *msg, unsigned flags)
      : Base{io_uring_prep_sendmsg, fd, msg, flags} {}

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result >= 0) [[likely]] {
      return static_cast<std::size_t>(this->_user_data.result);
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

class SendMsgZC : public IORegistrantAwaiter<SendMsgZC> {
private:
  using Base = IORegistrantAwaiter<SendMsgZC>;

public:
  SendMsgZC(int fd, const struct msghdr *msg, unsigned flags)
      : Base{io_uring_prep_sendmsg_zc, fd, msg, flags} {}

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result >= 0) [[likely]] {
      return static_cast<std::size_t>(this->_user_data.result);
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_SENDMSG_HPP