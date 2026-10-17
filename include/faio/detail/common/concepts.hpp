/**
 * @file faio/detail/common/concepts.hpp
 * @brief 公共类型、错误与基础工具：公共类型约束。
 * @details 供 faio 各模块复用的头文件接口；具体所有权、错误与线程访问约束见类型及成员说明。
 */
#ifndef FAIO_DETAIL_COMMON_CONCEPTS_HPP
#define FAIO_DETAIL_COMMON_CONCEPTS_HPP
#include <concepts>
#include <coroutine>
#include <span>
#include <sys/socket.h>

namespace faio {
template <typename Addr>
concept is_socket_address = requires(Addr addr) {
  { addr.sockaddr() } noexcept -> std::same_as<struct sockaddr*>;
  { addr.length() } noexcept -> std::same_as<socklen_t>;
};

template <class IOAwaiter>
concept is_awaiter = requires(IOAwaiter awaiter) {
  { awaiter.await_ready() };
  { awaiter.await_resume() };
  { awaiter.await_suspend(std::noop_coroutine()) };
};

template <typename C>
concept constructible_to_char_slice = requires(C c) { std::span<const char>{c}; };
}
#endif
