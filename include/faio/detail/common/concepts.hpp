#ifndef FAIO_DETAIL_COMMON_CONCEPTS_HPP
#define FAIO_DETAIL_COMMON_CONCEPTS_HPP
#include <concepts>
#include <coroutine>
#include <span>
#include <sys/socket.h>

namespace faio {

template <typename Addr>
concept is_socket_address = requires(Addr addr) {
  { addr.sockaddr() } noexcept -> std::same_as<struct sockaddr *>;
  { addr.length() } noexcept -> std::same_as<socklen_t>;
};

template <class IOAwaiter>
concept is_awaiter = requires(IOAwaiter awaiter) {
  { awaiter.await_ready() };
  { awaiter.await_resume() };
  { awaiter.await_suspend(std::noop_coroutine()) };
};

template <typename C>
concept constructible_to_char_slice =
    requires(C c) { std::span<const char>{c}; };

} // namespace faio
#endif // FAIO_DETAIL_COMMON_CONCEPTS_HPP