/**
 * @file faio/detail/io/platform/linux/backend_selection.hpp
 * @brief 跨平台异步 IO：编译期后端选择。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#if defined(__linux__)
#include <cerrno>
#include <charconv>
#include <string_view>
#include <sys/utsname.h>
#include <system_error>

namespace faio::runtime {
/** @brief Linux 后端标识；构建时固定一种，显式配置仅验证与构建匹配。 */
enum class io_backend : std::uint8_t { IO_EPOLL, IO_URING };
}

namespace faio::io::detail {
struct kernel_version {
  unsigned major{},  ///< 内核版本主版本号，用于初始化阶段能力判断。
      minor{};  ///< 内核版本次版本号，用于初始化阶段能力判断。
};

/** @brief 按数字读取 uname release；用于验证固定 uring 构建的最低内核版本。 */
inline kernel_version parse_kernel_version(std::string_view release) {
  kernel_version result;
  const auto first = std::from_chars(release.data(), release.data() + release.size(), result.major);
  if (first.ec != std::errc{} || first.ptr == release.data() + release.size() || *first.ptr != '.')
    throw std::invalid_argument("无法解析 Linux 内核版本");
  const auto second = std::from_chars(first.ptr + 1, release.data() + release.size(), result.minor);
  if (second.ec != std::errc{})
    throw std::invalid_argument("无法解析 Linux 内核次版本");
  return result;
}

#if defined(FAIO_HAS_IO_URING) && FAIO_HAS_IO_URING
inline constexpr auto compiled_io_backend = runtime::io_backend::IO_URING;
#else
inline constexpr auto compiled_io_backend = runtime::io_backend::IO_EPOLL;
#endif

inline runtime::io_backend resolve_io_backend(std::optional<runtime::io_backend> requested,
                                              [[maybe_unused]] kernel_version version) {
  if (requested && *requested != compiled_io_backend)
    throw std::invalid_argument(
        "IO 后端已在编译期固定；epoll 请构建 FAIO_ENABLE_IO_URING=OFF，uring 请构建 ON");
#if defined(FAIO_HAS_IO_URING) && FAIO_HAS_IO_URING
  if (!(version.major > 5 || (version.major == 5 && version.minor >= 10)))
    throw std::invalid_argument(
        "本构建固定 io_uring，需要 Linux >= 5.10；旧内核请重新构建 epoll 版本");
#endif
  return compiled_io_backend;
}

inline runtime::io_backend resolve_io_backend(std::optional<runtime::io_backend> requested) {
#if !defined(FAIO_HAS_IO_URING) || !FAIO_HAS_IO_URING
  /// @brief epoll 构建不需要运行时探测。
  return resolve_io_backend(requested, {});
#else
  utsname info{};
  if (::uname(&info) < 0)
    throw std::system_error(errno, std::generic_category(), "uname");
  return resolve_io_backend(requested, parse_kernel_version(info.release));
#endif
}
}
#endif
