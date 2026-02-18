#ifndef FAIO_DETAIL_IO_AWAITER_SHUTDOWN_HPP
#define FAIO_DETAIL_IO_AWAITER_SHUTDOWN_HPP

#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io {

enum class ShutdownBehavior {
  Read = SHUT_RD,
  Write = SHUT_WR,
  ReadWrite = SHUT_RDWR,
};
}

namespace faio::io::detail {
class Shutdown : public IORegistrantAwaiter<Shutdown> {
private:
  using Base = IORegistrantAwaiter<Shutdown>;

public:
  Shutdown(int fd, int how) : Base{io_uring_prep_shutdown, fd, how} {}

  Shutdown(int fd, ShutdownBehavior how)
      : Base{io_uring_prep_shutdown, fd, static_cast<int>(how)} {}

  auto await_resume() const noexcept -> expected<void> {
    if (this->_user_data.result >= 0) [[likely]] {
      return {};
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail
#endif // FAIO_DETAIL_IO_AWAITER_SHUTDOWN_HPP
