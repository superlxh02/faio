#ifndef FAIO_DETAIL_IO_AWAITER_OPEN_HPP
#define FAIO_DETAIL_IO_AWAITER_OPEN_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class Open : public IORegistrantAwaiter<Open> {
private:
  using Base = IORegistrantAwaiter<Open>;

public:
  Open(int dfd, const char *path, int flags, mode_t mode)
      : Base{io_uring_prep_openat, dfd, path, flags, mode} {}

  Open(const char *path, int flags, mode_t mode)
      : Open{AT_FDCWD, path, flags, mode} {}

  auto await_resume() const noexcept -> expected<int> {
    if (this->_user_data.result >= 0) [[likely]] {
      return this->_user_data.result;
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

class Open2 : public IORegistrantAwaiter<Open2> {
private:
  using Base = IORegistrantAwaiter<Open2>;

public:
  Open2(int dfd, const char *path, struct open_how *how)
      : Base{io_uring_prep_openat2, dfd, path, how} {}

  Open2(const char *path, struct open_how *how) : Open2{AT_FDCWD, path, how} {}

  auto await_resume() const noexcept -> expected<int> {
    if (this->_user_data.result >= 0) [[likely]] {
      return this->_user_data.result;
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_OPEN_HPP