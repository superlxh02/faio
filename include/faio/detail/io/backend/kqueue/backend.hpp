/**
 * @file faio/detail/io/backend/kqueue/backend.hpp
 * @brief 跨平台异步 IO：kqueue 就绪后端：后端封装与提交协议。
 * @details 统一后端只通过资源键或操作代际返回事件；控制回执与业务完成分别处理。内核解除请求与负载引用后才允许复用稳定存储，唤醒使用独立控制通道。
 */
#pragma once
#include "faio/detail/io/backend/kqueue/reactor.hpp"
#include "faio/detail/io/base/reactor.hpp"
#include "faio/detail/io/base/readiness_adapter.hpp"

namespace faio::io::detail {
#if defined(__APPLE__) || defined(__FreeBSD__)
/// @brief kqueue 的具体 readiness 后端类型，不负责选择平台或构造拥有者。
using kqueue_backend = basic_readiness_adapter<basic_reactor_box<kqueue_reactor>>;
#endif
}
