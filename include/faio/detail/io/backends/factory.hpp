#pragma once
#include "faio/detail/io/backend_selection.hpp"
#include "faio/detail/io/backends/epoll/backend.hpp"
#include "faio/detail/io/backends/iocp/backend.hpp"
#include "faio/detail/io/backends/kqueue/backend.hpp"
#include "faio/detail/io/backends/uring/backend.hpp"
namespace faio::io::detail {
/**
 * @brief 所有平台的后端创建边界，返回同一个 owning backend_box。
 * @param ring_capacity 原生 SQ 容量；reactor
 * 不使用它，也不把它当作最大在途操作数。
 * @details Linux >=5.10 且编译启用 uring 时默认原生后端，旧内核默认 epoll；
 *          显式 uring 的初始化/内核/编译能力错误均向上诊断，禁止偷偷切换语义。
 *          macOS/BSD 选择 kqueue；Windows 原生 IOCP
 * 框架使用同一协议并明确拒绝实际 IO。
 */
inline backend_box make_platform_backend(
#if defined(__linux__)
    std::optional<runtime::io_backend> requested = {},
#endif
    [[maybe_unused]] unsigned ring_capacity = 256) {
#if defined(__linux__)
  if (resolve_io_backend(requested) ==
      runtime::io_backend::IO_URING) { // 只在初始化读取 uname，热路径固定归属。
#if defined(FAIO_HAS_IO_URING) && FAIO_HAS_IO_URING
    return backend_box{std::make_unique<uring_backend>(
        ring_capacity)}; // Proactor 原生完成不经过 readiness syscall adapter。
#else
    throw std::invalid_argument("io_uring 未在本构建启用；重新构建 "
                                "FAIO_ENABLE_IO_URING=ON 或显式 IO_EPOLL");
#endif
  }
  return make_epoll_backend(); // reactor 经 adapter
                               // 汇入同一核心准备/取消/发布/回收协议。
#elif defined(__APPLE__) || defined(__FreeBSD__)
  (void)ring_capacity;
  return make_kqueue_backend(); // EV_CLEAR 与尝试 syscall/EAGAIN
                                // 重试归属保持在公共 domain。
#elif defined(_WIN32)
  (void)ring_capacity;
  auto backend = windows::make_iocp_backend(); // 框架 factory 不伪装成已完成
                                               // Windows 适配。
  if (!backend)
    throw std::system_error(backend.error(), "IOCP 尚未实现实际 IO");
  return std::move(*backend);
#else
  (void)ring_capacity;
  throw std::invalid_argument("本平台没有可用 IO 后端");
#endif
}
} // namespace faio::io::detail
