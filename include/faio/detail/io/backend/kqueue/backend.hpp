/**
 * @file faio/detail/io/backend/kqueue/backend.hpp
 * @brief kqueue 就绪后端：后端封装与提交协议。
 */
#pragma once
#include "faio/detail/io/backend/kqueue/reactor.hpp"
#include "faio/detail/io/base/reactor.hpp"
#include "faio/detail/io/base/readiness_adapter.hpp"

namespace faio::io::detail {
#if defined(__APPLE__) || defined(__FreeBSD__)
/** @brief kqueue 的具体就绪后端类型，不负责平台选择或对象所有权管理。 */
using kqueue_backend = basic_readiness_adapter<basic_reactor_box<kqueue_reactor>>;
#endif
}  // namespace faio::io::detail
