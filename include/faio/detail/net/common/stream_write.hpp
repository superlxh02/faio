#ifndef FAIO_DETAIL_NET_COMMON_STREAM_WRITE_HPP
#define FAIO_DETAIL_NET_COMMON_STREAM_WRITE_HPP

#include "faio/detail/common/concepts.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/io.hpp"
#include <bits/types/struct_iovec.h>
#include <cstddef>
#include <utility>

namespace faio::net::detail {

template <class T> struct ImplStreamWrite {
  // 基础的写入操作
  auto write(std::span<const char> buf) noexcept {
    return io::detail::Send{static_cast<T *>(this)->fd(), buf.data(),
                            buf.size_bytes(), MSG_NOSIGNAL};
  }
  // 零拷贝写入操作
  auto write_zc(std::span<const char> buf) noexcept {
    return io::detail::SendZC{static_cast<T *>(this)->fd(), buf.data(),
                              buf.size_bytes(), MSG_NOSIGNAL, 0};
  }
  // 分散写入操作
  template <typename... Ts>
    requires(constructible_to_char_slice<Ts> && ...)
  auto write_v(Ts &&...bufs) noexcept {
    constexpr auto N = sizeof...(Ts);

    class WriteV : public io::detail::IORegistrantAwaiter<WriteV> {
    private:
      using Base = io::detail::IORegistrantAwaiter<WriteV>;

    public:
      WriteV(int fd, Ts &&...bufs)
          : Base{io_uring_prep_sendmsg, fd, &_msg, MSG_NOSIGNAL},
            _iovecs{iovec{
                .iov_base =
                    const_cast<char *>(std::span<const char>(bufs).data()),
                .iov_len = std::span<const char>(bufs).size_bytes(),
            }...},
            _msg{.msg_name = nullptr,
                 .msg_namelen = 0,
                 .msg_iov = _iovecs.data(),
                 .msg_iovlen = N,
                 .msg_control = nullptr,
                 .msg_controllen = 0,
                 .msg_flags = MSG_NOSIGNAL} {}

      auto await_resume() const noexcept -> expected<std::size_t> {
        if (this->_user_data.result >= 0) [[likely]] {
          return static_cast<std::size_t>(this->_user_data.result);
        } else {
          return ::std::unexpected{make_error(-this->_user_data.result)};
        }
      }

    private:
      std::array<struct iovec, N> _iovecs;
      struct msghdr _msg;
    };
    return WriteV{static_cast<T *>(this)->fd(), std::forward<Ts>(bufs)...};
  }

  // 保证写入指定字节数的字节
  task<expected<void>> write_all(std::span<const char> buf) noexcept {
    expected<std::size_t> res{0};
    while (!buf.empty()) {
      res = co_await this->write(buf);
      if (!res) {
        co_return std::unexpected{std::move(res.error())};
      }
      if (res.value() == 0) {
        co_return std::unexpected{Error{Error::WriteZero}};
      }
      buf = buf.subspan(res.value(), buf.size_bytes() - res.value());
    }
    co_return expected<void>{};
  }
};
} // namespace faio::net::detail

#endif // FAIO_DETAIL_NET_COMMON_STREAM_WRITE_HPP