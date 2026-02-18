#ifndef FAIO_DETAIL_IO_AWAITER_READV_HPP
#define FAIO_DETAIL_IO_AWAITER_READV_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class ReadV : public IORegistrantAwaiter<ReadV> {
private:
  using Base = IORegistrantAwaiter<ReadV>;

public:
  ReadV(int fd, const struct iovec *iovecs, unsigned nr_vecs, __u64 offset,
        int flags)
      : Base{io_uring_prep_readv2, fd, iovecs, nr_vecs, offset, flags} {}

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result >= 0) [[likely]] {
      return static_cast<std::size_t>(this->_user_data.result);
    } else {
      return std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_READV_HPP