/**
 * @file faio/detail/io/platform/reactor.hpp
 * @brief 就绪反应器。
 */
#pragma once
#include "faio/detail/io/base/reactor.hpp"

#if defined(FAIO_TEST_BACKEND_INJECTION)
#include "faio_test/reactor_override.hpp"
#else
namespace faio::io::detail {
#if defined(__linux__)
/** @brief Linux epoll 就绪反应器。 */
class epoll_reactor;
using reactor_box = basic_reactor_box<epoll_reactor>;
#elif defined(__APPLE__) || defined(__FreeBSD__)
/** @brief BSD kqueue 就绪反应器。 */
class kqueue_reactor;
using reactor_box = basic_reactor_box<kqueue_reactor>;
#endif
}  // namespace faio::io::detail
#endif

#if !defined(_WIN32)
namespace faio::io::detail {
/** @brief 构造当前平台的就绪反应器。 */
reactor_box make_platform_reactor();
}  // namespace faio::io::detail
#endif
