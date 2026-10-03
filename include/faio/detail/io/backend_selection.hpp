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
/** @brief Linux 同一发行二进制同时提供两个后端，显式选择不会自动回退。 */
enum class io_backend : std::uint8_t { IO_EPOLL, IO_URING };
} // namespace faio::runtime
namespace faio::io::detail {
struct kernel_version {
  unsigned major{}, minor{};
};
/** @brief 按数字读取 uname release；5.10及更新内核默认原生 io_uring。 */
inline kernel_version parse_kernel_version(std::string_view release) {
  kernel_version result;
  const auto first = std::from_chars(
      release.data(), release.data() + release.size(), result.major);
  if (first.ec != std::errc{} || first.ptr == release.data() + release.size() ||
      *first.ptr != '.')
    throw std::invalid_argument("无法解析 Linux 内核版本");
  const auto second = std::from_chars(
      first.ptr + 1, release.data() + release.size(), result.minor);
  if (second.ec != std::errc{})
    throw std::invalid_argument("无法解析 Linux 内核次版本");
  return result;
}
inline runtime::io_backend
resolve_io_backend(std::optional<runtime::io_backend> requested,
                   kernel_version version) {
#if !defined(FAIO_HAS_IO_URING) || !FAIO_HAS_IO_URING
  if (requested == runtime::io_backend::IO_URING)
    throw std::invalid_argument(
        "io_uring 未编译；请设置 FAIO_ENABLE_IO_URING=ON 或使用 IO_EPOLL");
  (void)version;
  return requested.value_or(runtime::io_backend::IO_EPOLL);
#else
  const bool supported =
      version.major > 5 || (version.major == 5 && version.minor >= 10);
  if (requested == runtime::io_backend::IO_URING && !supported)
    throw std::invalid_argument(
        "io_uring 需要 Linux >= 5.10；请使用 set_io_backend(IO_EPOLL)");
  return requested.value_or(supported ? runtime::io_backend::IO_URING
                                      : runtime::io_backend::IO_EPOLL);
#endif
}
inline runtime::io_backend
resolve_io_backend(std::optional<runtime::io_backend> requested) {
  utsname info{};
  if (::uname(&info) < 0)
    throw std::system_error(errno, std::generic_category(), "uname");
  return resolve_io_backend(requested, parse_kernel_version(info.release));
}
} // namespace faio::io::detail
#endif
