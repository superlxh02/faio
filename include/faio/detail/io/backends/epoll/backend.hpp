#pragma once
#include "faio/detail/io/backends/epoll/reactor.hpp"
#include "faio/detail/io/reactor/readiness_adapter.hpp"
namespace faio::io::detail {
#if defined(__linux__)
/** @brief Linux reactor 使用中立后端函数表；网络 syscall 与文件 fallback 均由
 * core 调度。 */
inline backend_box make_epoll_backend() {
  return backend_box{std::make_unique<readiness_adapter>(
      reactor_box{std::make_unique<epoll_reactor>()})}; // 两层 owning RAII
                                                        // 不依赖 TLS 析构。
}
#endif
} // namespace faio::io::detail
