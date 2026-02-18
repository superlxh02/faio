#ifndef FAIO_DETAIL_IO_AWAITER_CANCEL_HPP
#define FAIO_DETAIL_IO_AWAITER_CANCEL_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class Cancel : public IORegistrantAwaiter<Cancel> {
private:
  using Base = IORegistrantAwaiter<Cancel>;

public:
  Cancel(int fd, unsigned int flags)
      : Base{io_uring_prep_cancel_fd, fd, flags} {}

  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result >= 0) {
      return {};
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_CANCEL_HPP