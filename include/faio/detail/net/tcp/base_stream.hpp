#ifndef FAIO_DETAIL_NET_TCP_BASE_STREAM_HPP
#define FAIO_DETAIL_NET_TCP_BASE_STREAM_HPP

#include "faio/detail/net/common/addr_util.hpp"
#include "faio/detail/net/common/socket.hpp"
#include "faio/detail/net/common/sockopt.hpp"
#include "faio/detail/net/common/stream_read.hpp"
#include "faio/detail/net/common/stream_write.hpp"
namespace faio::net::detail {
template <class Stream, class Addr>
class BaseStream : public ImplStreamRead<BaseStream<Stream, Addr>>,
                   public ImplStreamWrite<BaseStream<Stream, Addr>>,
                   public ImplLocalAddr<BaseStream<Stream, Addr>, Addr>,
                   public ImplPeerAddr<BaseStream<Stream, Addr>, Addr> {
public:
protected:
  explicit BaseStream(Socket &&inner) : _inner_socket{std::move(inner)} {}

public:
  auto shutdown(io::ShutdownBehavior how) noexcept {
    return _inner_socket.shutdown(how);
  }

  auto close() noexcept { return _inner_socket.close(); }

  [[nodiscard]]
  auto fd() const noexcept {
    return _inner_socket.fd();
  }

public:
  static auto connect(const Addr &addr) {
    class Connect : public io::detail::IORegistrantAwaiter<Connect> {

    private:
      using Base = io::detail::IORegistrantAwaiter<Connect>;

    public:
      Connect(const Addr &addr)
          : Base{io_uring_prep_connect, -1, nullptr, sizeof(Addr)},
            addr_{addr} {}

      auto await_suspend(std::coroutine_handle<> handle) -> bool {
        fd_ = ::socket(addr_.family(), SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd_ < 0) [[unlikely]] {
          this->_user_data.result = errno;
          io_uring_prep_nop(this->_sqe);
          io_uring_sqe_set_data(this->_sqe, nullptr);
          return false;
        }
        this->_sqe->fd = fd_;
        this->_sqe->addr = (unsigned long)addr_.sockaddr();
        Base::await_suspend(handle);
        return true;
      }

      auto await_resume() noexcept -> expected<Stream> {
        if (this->_user_data.result >= 0) [[likely]] {
          return Stream{Socket{fd_}};
        } else {
          if (fd_ >= 0) {
            ::close(fd_);
          }
          return std::unexpected{make_error(-this->_user_data.result)};
        }
      }

    private:
      int fd_;
      Addr addr_;
    };
    return Connect{addr};
  }

private:
  Socket _inner_socket;
};
} // namespace faio::net::detail
#endif // FAIO_DETAIL_NET_TCP_BASE_STREAM_HPP