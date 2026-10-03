#pragma once
#include "faio/detail/io/backends/kqueue/reactor.hpp"
#include "faio/detail/io/reactor/readiness_adapter.hpp"
namespace faio::io::detail {
#if defined(__APPLE__) || defined(__FreeBSD__)
/** @brief Darwin/BSD reactor 与 epoll 使用同一个 readiness adapter 和 operation
 * 协议。 */
inline backend_box make_kqueue_backend() {
  return backend_box{std::make_unique<readiness_adapter>(reactor_box{
      std::make_unique<kqueue_reactor>()})}; // kqueue 仅产生资源
                                             // readiness，核心发布业务完成。
}
#endif
} // namespace faio::io::detail
