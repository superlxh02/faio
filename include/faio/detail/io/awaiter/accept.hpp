#ifndef FAIO_DETAIL_IO_AWAITER_ACCEPT_HPP
#define FAIO_DETAIL_IO_AWAITER_ACCEPT_HPP

#include "faio/detail/common/error.hpp"
#include "faio/detail/io/base/io_registrant.hpp"

namespace faio::io::detail {

class Accept : public IORegistrantAwaiter<Accept> {
private:
  using Base = IORegistrantAwaiter<Accept>;

public:
  Accept(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
      : Base{io_uring_prep_accept, fd, addr, addrlen, flags} {}

  // 恢复逻辑，取结果返回结果
  auto await_resume() const noexcept -> expected<int> {
    if (this->_user_data.result >= 0) {
      return this->_user_data.result;
    } else {
      return ::std::unexpected{make_error(-this->_user_data.result)};
    }
  }
};

} // namespace faio::io::detail

#endif // FAIO_DETAIL_IO_AWAITER_ACCEPT_HPP