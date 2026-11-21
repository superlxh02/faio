/**
 * @file faio/detail/io/awaiter/open.hpp
 * @brief 打开文件。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <fcntl.h>

namespace faio::io::detail {
class Open : public IORegistrantAwaiter<Open> {
  using Base = IORegistrantAwaiter<Open>;

 public:
  /** @brief 构造文件打开请求。 */
  Open(native_descriptor directory, const char* path, int flags, mode_t mode)
      : Base{[&] {
          io_request r;
          r.kind = operation_kind::open;
#if defined(_WIN32)
          r.native_kind = native_handle_kind::windows_handle;
          r.bypass_resource_registration = true;
#endif
          r.fd = directory;
          r.path = path ? path : "";
          r.flags = flags;
          r.argument = static_cast<int>(mode);
          return r;
        }()} {
  }

  /** @brief 构造文件打开请求。 */
  Open(const char* path, int flags, mode_t mode) : Open{AT_FDCWD, path, flags, mode} {}

#if defined(_WIN32)
  /** @brief 取得异步操作结果。 */
  auto await_resume() const noexcept -> expected<HANDLE>{

#else
  auto await_resume() const noexcept -> expected<native_descriptor> {
#endif
      if (this->_user_data.result < 0) return std::unexpected {
        decode_io_error(static_cast<int>(-this->_user_data.result), this->_user_data.transferred)
      };
#if defined(_WIN32)
  return reinterpret_cast<HANDLE>(static_cast<native_descriptor>(this->_user_data.result));
#else
    return static_cast<native_descriptor>(this->_user_data.result);
#endif
}
};  // namespace faio::io::detail
}  // namespace faio::io::detail

#if defined(__linux__)
#include "faio/detail/io/platform/linux/open.hpp"
#endif

namespace faio::io {
inline auto open(const char* path, int flags, mode_t mode = 0666) {
  return detail::Open{path, flags, mode};
}

inline auto openat(detail::native_descriptor directory,
                   const char* path,
                   int flags,
                   mode_t mode = 0666) {
  return detail::Open{directory, path, flags, mode};
}
}  // namespace faio::io
