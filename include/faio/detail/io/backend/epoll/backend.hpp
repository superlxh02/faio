/**
 * @file faio/detail/io/backend/epoll/backend.hpp
 * @brief epoll 就绪后端：后端封装与提交协议。
 */
#pragma once
#include "faio/detail/io/backend/epoll/reactor.hpp"
#include "faio/detail/io/base/reactor.hpp"
#include "faio/detail/io/base/readiness_adapter.hpp"

namespace faio::io::detail {
#if defined(__linux__)
/** @brief epoll 的具体就绪后端类型，不负责平台选择或对象所有权管理。 */
using epoll_backend = basic_readiness_adapter<basic_reactor_box<epoll_reactor>>;
#endif
}  // namespace faio::io::detail
