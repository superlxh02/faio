/**
 * @file faio/detail/io/platform/backend.hpp
 * @brief 后端封装与提交协议。
 */
#pragma once
#include "faio/detail/io/base/backend.hpp"
#include "faio/detail/io/platform/reactor.hpp"

namespace faio::io::detail {
/** @brief 将原生请求映射为就绪等待的适配器。 */
template <class R>
class basic_readiness_adapter;
/** @brief 就绪反应器的静态封装。 */
template <class R>
class basic_reactor_box;
/** @brief Linux epoll 就绪反应器。 */
class epoll_reactor;
/** @brief BSD kqueue 就绪反应器。 */
class kqueue_reactor;
/** @brief Linux io_uring 原生完成后端。 */
class uring_backend;
#if !defined(_WIN32) || defined(FAIO_TEST_BACKEND_INJECTION)
using readiness_adapter = basic_readiness_adapter<reactor_box>;
#endif
}  // namespace faio::io::detail

namespace faio::io::windows {
/** @brief Windows IOCP 原生完成后端。 */
class iocp_backend;
}  // namespace faio::io::windows

#if defined(FAIO_TEST_BACKEND_INJECTION)
#include "faio_test/backend_override.hpp"
#else
namespace faio::io::detail {
#if defined(__linux__) && defined(FAIO_HAS_IO_URING) && FAIO_HAS_IO_URING
using backend_box = basic_backend_box<uring_backend>;
#elif defined(__linux__)
using backend_box = basic_backend_box<basic_readiness_adapter<basic_reactor_box<epoll_reactor>>>;
#elif defined(__APPLE__) || defined(__FreeBSD__)
using backend_box = basic_backend_box<basic_readiness_adapter<basic_reactor_box<kqueue_reactor>>>;
#elif defined(_WIN32)
using backend_box = basic_backend_box<windows::iocp_backend>;
#endif
}  // namespace faio::io::detail
#endif
