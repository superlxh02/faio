#ifndef FAIO_DETAIL_IO_AWAITER_WRITE_HPP
#define FAIO_DETAIL_IO_AWAITER_WRITE_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class Write : public IORegistrantAwaiter<Write> {
private:
  using Base = IORegistrantAwaiter<Write>;

public:
  Write(int fd, const void *buf, unsigned nbytes, __u64 offset)
      : Base{io_uring_prep_write, fd, buf, nbytes, offset} {}

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result >= 0) [[likely]] {
      return static_cast<std::size_t>(this->_user_data.result);
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_WRITE_HPP