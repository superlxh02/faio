#ifndef FAIO_DETAIL_NET_COMMON_STREAM_READ_HPP
#define FAIO_DETAIL_NET_COMMON_STREAM_READ_HPP
#include "faio/detail/common/concepts.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/io.hpp"
#include <bits/types/struct_iovec.h>
#include <cstddef>
#include <utility>

namespace faio::net::detail {
template <class T> struct ImplStreamRead {
  // 基础的读取操作
  auto read(std::span<char> buf) const noexcept {
    return io::detail::Read{static_cast<const T *>(this)->fd(), buf.data(),
                            buf.size(), 0};
  }

  // 分散读取
  template <class... Ts>
    requires(constructible_to_char_slice<Ts> && ...)
  auto read_v(Ts &&...buffers) const noexcept {
    constexpr std::size_t N = sizeof...(Ts);
    class ReadV : public io::detail::IORegistrantAwaiter<ReadV> {

    private:
      using Base = io::detail::IORegistrantAwaiter<ReadV>;

    public:
      ReadV(int fd, Ts &...buffers)
          : Base{io_uring_prep_readv, fd, nullptr, N,
                 static_cast<std::size_t>(-1)},
            _iovecs(
                iovec{.iov_base = std::span<char>(buffers).data(),
                      .iov_len = std::span<char>(buffers).size_bytes()}...) {
        this->_sqe = reinterpret_cast<unsigned long long>(_iovecs.data());
      }

      auto await_resume() const noexcept -> expected<std::size_t> {
        if (this->_user_data.result >= 0) {
          return static_cast<std::size_t>(this->_user_data.result);
        } else {
          return ::std::unexpected{make_error(-this->_user_data.result)};
        }
      }

    private:
      std::array<struct iovec, N> _iovecs;
    };
    return ReadV{static_cast<T *>(this)->fd(), std::forward<Ts>(buffers)...};
  }

  // 保证读取指定字节数的字节
  task<expected<void>> read_bytes(std::span<char> buf) const noexcept {
    expected<std::size_t> res{0};
    while (!buf.empty()) {
      res = co_await this->read(buf);
      if (!res) {
        co_return std::unexpected{std::move(res.error())};
      }
      if (res.value() == 0) {
        co_return std::unexpected{Error{Error::UnexpectedEOF}};
      }
      buf = buf.subspan(res.value(), buf.size_bytes() - res.value());
    }
    co_return expected<void>{};
  }

  // 预读取指定字节数的字节
  auto peek(std::span<char> buf) const noexcept {
    return io::detail::Recv{static_cast<const T *>(this)->fd(), buf.data(),
                            buf.size_bytes(), MSG_PEEK};
  }
};

} // namespace faio::net::detail

#endif // FAIO_DETAIL_NET_COMMON_STREAM_READ_HPP