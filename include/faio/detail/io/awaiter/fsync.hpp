#ifndef FAIO_DETAIL_IO_AWAITER_FSYNC_HPP
#define FAIO_DETAIL_IO_AWAITER_FSYNC_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class Fsync : public IORegistrantAwaiter<Fsync> {
private:
  using Base = IORegistrantAwaiter<Fsync>;

public:
  Fsync(int fd, unsigned fsync_flags)
      : Base{io_uring_prep_fsync, fd, fsync_flags} {}

  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result >= 0) [[likely]] {
      return {};
    } else {
      return std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_FSYNC_HPP