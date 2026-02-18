#ifndef FAIO_DETAIL_IO_AWAITER_CLOSE_HPP
#define FAIO_DETAIL_IO_AWAITER_CLOSE_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class Close : public IORegistrantAwaiter<Close> {
private:
  using Base = IORegistrantAwaiter<Close>;

public:
  Close(int fd) : Base{io_uring_prep_close, fd} {}

  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result >= 0) {
      return {};
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_CLOSE_HPP