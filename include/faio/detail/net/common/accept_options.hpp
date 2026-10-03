#ifndef FAIO_DETAIL_NET_COMMON_ACCEPT_OPTIONS_HPP
#define FAIO_DETAIL_NET_COMMON_ACCEPT_OPTIONS_HPP
#include "faio/detail/io/context.hpp"

namespace faio::net {
/** @brief 新接受连接的首次 IO 归属策略；已经发布的连接不会迁移 domain。 */
enum class accept_placement {
  listener_local,   ///< 绑定 listener 的实际 IO domain，适合显式亲和性控制。
  balanced,         ///< 在同一 runtime 的活跃 IO domain 中轮转分配；独立引擎使用自身。
  explicit_context  ///< 绑定 accept_options::target 指定的 domain。
};

/** @brief TCP/Unix listener 共用的接受配置；默认分散新连接的 reactor
 * 和状态锁负载。
 * @details target 只在 explicit_context 策略下使用，必须是非空且未停止的
 * context。 选择与注册发生在返回 stream 前；后续协程移动不改变连接的稳定归属。
 */
struct accept_options {
  accept_placement placement{accept_placement::balanced};  ///< 新 fd 的一次性归属策略。
  io::io_context target{};  ///< 显式归属的拥有型服务租约，不借用 worker 指针。
};
}  // namespace faio::net
#endif  // FAIO_DETAIL_NET_COMMON_ACCEPT_OPTIONS_HPP
