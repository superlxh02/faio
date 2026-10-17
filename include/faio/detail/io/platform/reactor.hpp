/**
 * @file faio/detail/io/platform/reactor.hpp
 * @brief 跨平台异步 IO：就绪反应器。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#include "faio/detail/io/base/reactor.hpp"

#if defined(FAIO_TEST_BACKEND_INJECTION)
#include "faio_test/reactor_override.hpp"
#else
namespace faio::io::detail {
#if defined(__linux__)
class epoll_reactor;
using reactor_box = basic_reactor_box<epoll_reactor>;
#elif defined(__APPLE__) || defined(__FreeBSD__)
class kqueue_reactor;
using reactor_box = basic_reactor_box<kqueue_reactor>;
#endif
}
#endif

#if !defined(_WIN32)
namespace faio::io::detail {
reactor_box make_platform_reactor();
}
#endif
