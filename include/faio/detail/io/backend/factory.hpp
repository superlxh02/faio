/**
 * @file faio/detail/io/backend/factory.hpp
 * @brief 跨平台异步 IO：平台后端工厂：平台后端工厂。
 * @details 统一后端只通过资源键或操作代际返回事件；控制回执与业务完成分别处理。内核解除请求与负载引用后才允许复用稳定存储，唤醒使用独立控制通道。
 */
#pragma once
#include "faio/detail/io/backend/epoll/backend.hpp"
#include "faio/detail/io/backend/iocp/backend.hpp"
#include "faio/detail/io/backend/kqueue/backend.hpp"
#include "faio/detail/io/backend/uring/backend.hpp"
#include "faio/detail/io/platform/backend.hpp"
#include "faio/detail/io/platform/linux/backend_selection.hpp"
#include "faio/detail/io/platform/reactor.hpp"
#include <expected>
#include <memory>
#include <optional>
#include <system_error>

#if defined(_WIN32)
namespace faio::io::windows {
using backend_box = ::faio::io::detail::backend_box;

/** @brief 初始化 IOCP 并建立选定拥有者；失败保留原生错误。 */
inline std::expected<backend_box, std::error_code> make_iocp_backend() noexcept {
  try {
    return backend_box{std::make_unique<iocp_backend>()};
  } catch (const std::system_error& error) {
    return std::unexpected{error.code()};
  } catch (...) {
    return std::unexpected{std::make_error_code(std::errc::not_enough_memory)};
  }
}
}
#endif

namespace faio::io::detail {
#if defined(__linux__)
/// @brief 具体 epoll 拥有者，供平台选择和独立 backend 合同共用。
inline auto make_epoll_backend() {
  using reactor = basic_reactor_box<epoll_reactor>;
  return basic_backend_box<epoll_backend>{
      std::make_unique<epoll_backend>(reactor{std::make_unique<epoll_reactor>()})};
}

inline reactor_box make_platform_reactor() {
  return reactor_box{std::make_unique<epoll_reactor>()};
}
#elif defined(__APPLE__) || defined(__FreeBSD__)
/// @brief 具体 kqueue 拥有者，供平台选择和独立 backend 合同共用。
inline auto make_kqueue_backend() {
  using reactor = basic_reactor_box<kqueue_reactor>;
  return basic_backend_box<kqueue_backend>{
      std::make_unique<kqueue_backend>(reactor{std::make_unique<kqueue_reactor>()})};
}

inline reactor_box make_platform_reactor() {
  return reactor_box{std::make_unique<kqueue_reactor>()};
}
#endif

/** @brief 创建编译期选定的具体后端；配置只验证匹配，不改变热路径类型。
 * @details Linux FAIO_HAS_IO_URING=1 固定 uring，否则固定 epoll；
 * macOS/BSD 固定 kqueue，Windows 固定 IOCP。初始化错误不静默降级。
 */
inline backend_box make_platform_backend(
#if defined(__linux__)
    std::optional<runtime::io_backend> requested = {},
#endif
    [[maybe_unused]] unsigned ring_capacity = 256) {
#if defined(__linux__)
  (void)resolve_io_backend(requested);
#if defined(FAIO_HAS_IO_URING) && FAIO_HAS_IO_URING
  return backend_box{std::make_unique<uring_backend>(ring_capacity)};
#else
  return backend_box{make_epoll_backend()};
#endif
#elif defined(__APPLE__) || defined(__FreeBSD__)
  return backend_box{make_kqueue_backend()};
#elif defined(_WIN32)
  auto backend = windows::make_iocp_backend();
  if (!backend)
    throw std::system_error(backend.error(), "IOCP 初始化失败");
  return std::move(*backend);
#else
#error "faio has no IO backend for this platform"
#endif
}
}
