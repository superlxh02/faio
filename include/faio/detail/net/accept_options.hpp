/**
 * @file faio/detail/net/accept_options.hpp
 * @brief 接受连接配置。
 */
#ifndef FAIO_DETAIL_NET_ACCEPT_OPTIONS_HPP
#define FAIO_DETAIL_NET_ACCEPT_OPTIONS_HPP
#include "faio/detail/io/core/context.hpp"

namespace faio::net {
/**  @brief 新接受连接的首次 IO 归属策略；已经发布的连接不会迁移 domain。 */
enum class accept_placement { LISTENER_LOCAL, BALANCED, EXPLICIT_CONTEXT };

/**
 *  @brief TCP/Unix listener 共用的接受配置；默认分散新连接的 reactor
 * 和状态锁负载。
 * @details target 只在 explicit_context 策略下使用，必须是非空且未停止的
 * context。 选择与注册发生在返回 stream 前；后续协程移动不改变连接的稳定归属。
 */
struct accept_options {
  accept_placement placement{accept_placement::BALANCED}; /**< 新 fd 首次注册时的归属策略。 */
  io::io_context target{}; /**< 显式归属的拥有型服务租约，不借用 worker 指针。 */
};
}  // namespace faio::net
#endif
