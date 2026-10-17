/**
 * @file faio/detail/io/platform/backend.hpp
 * @brief 跨平台异步 IO：后端封装与提交协议。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#include "faio/detail/io/base/backend.hpp"
#include "faio/detail/io/platform/reactor.hpp"

namespace faio::io::detail {
template <class R>
class basic_readiness_adapter;
template <class R>
class basic_reactor_box;
class epoll_reactor;
class kqueue_reactor;
class uring_backend;
#if !defined(_WIN32) || defined(FAIO_TEST_BACKEND_INJECTION)
using readiness_adapter = basic_readiness_adapter<reactor_box>;
#endif
}

namespace faio::io::windows {
class iocp_backend;
}

#if defined(FAIO_TEST_BACKEND_INJECTION)
/// @brief 仅单 TU 合同测试提供此头；安装消费方/生产目标不包含动态测试 seam。
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
}
#endif
