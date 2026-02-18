#ifndef FAIO_DETAIL_NET_TCP_BASE_LISTENER_HPP
#define FAIO_DETAIL_NET_TCP_BASE_LISTENER_HPP
#include "faio/detail/common/error.hpp"
#include "faio/detail/net/common/addr_util.hpp"
#include "faio/detail/net/common/socket.hpp"
#include "fastlog/fastlog.hpp"
namespace faio::net::detail {
template <class Listener, class Stream, class Addr>
class BaseListener
    : public detail::ImplLocalAddr<BaseListener<Listener, Stream, Addr>, Addr> {

protected:
  explicit BaseListener(Socket &&inner) : _inner_socket{std::move(inner)} {}

public:
  auto accept() const noexcept {
    class Accept : public io::detail::IORegistrantAwaiter<Accept> {
      using Base = io::detail::IORegistrantAwaiter<Accept>;

    public:
      Accept(int fd)
          : Base{io_uring_prep_accept, fd,
                 reinterpret_cast<struct sockaddr *>(&addr_), &length_,
                 SOCK_NONBLOCK} {}

      auto await_resume() const noexcept -> expected<std::pair<Stream, Addr>> {
        if (this->_user_data.result >= 0) [[likely]] {
          return std::make_pair(Stream{Socket{this->_user_data.result}}, addr_);
        } else {
          return std::unexpected{make_error(-this->_user_data.result)};
        }
      }

    private:
      Addr addr_{};
      socklen_t length_{sizeof(Addr)};
    };
    return Accept{fd()};
  }

  auto close() noexcept { return _inner_socket.close(); }

  [[nodiscard]]
  auto fd() const noexcept {
    return _inner_socket.fd();
  }

public:
  [[nodiscard]]
  static auto bind(const Addr &addr) -> expected<Listener> {
    auto ret = Socket::create(addr.family(), SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (!ret) [[unlikely]] {
      return std::unexpected{ret.error()};
    }
    auto &inner = ret.value();

    if (auto ret = inner.bind(addr); !ret) [[unlikely]] {
      return std::unexpected{ret.error()};
    }
    if (auto ret = inner.listen(); !ret) [[unlikely]] {
      return std::unexpected{ret.error()};
    }
    return Listener{std::move(inner)};
  }

  [[nodiscard]]
  static auto bind(const std::span<Addr> &addresses) -> expected<Listener> {
    for (const auto &address : addresses) {
      if (auto ret = bind(address); ret) [[likely]] {
        return ret;
      } else {
        fastlog::console.error("Bind {} failed, error: {}", address.to_string(),
                               ret.error().message());
      }
    }
    return std::unexpected{make_error(Error::InvalidAddresses)};
  }

private:
  Socket _inner_socket;
};
} // namespace faio::net::detail
#endif // FAIO_DETAIL_NET_TCP_BASE_LISTENER_HPP