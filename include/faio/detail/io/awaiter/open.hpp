#pragma once
#include "faio/detail/io/base/io_registrant.hpp"
#include <fcntl.h>

namespace faio::io::detail {
/** @brief Open 请求；参数由稳定 operation_state 保存，挂起前没有内核副作用。 */
class Open : public IORegistrantAwaiter<Open> {
  using Base = IORegistrantAwaiter<Open>;

 public:
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

  Open(const char* path, int flags, mode_t mode) : Open{AT_FDCWD, path, flags, mode} {}

#if defined(_WIN32)
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
#include <linux/openat2.h>

namespace faio::io::detail {
/** @brief Linux openat2 扩展，open_how 参数按值拥有，磁盘调用交给文件服务。 */
class Open2 : public IORegistrantAwaiter<Open2> {
 public:
  Open2(int directory, const char* path, const open_how* how)
      : IORegistrantAwaiter{[&] {
          io_request request;
          request.kind = how ? operation_kind::open2 : operation_kind::unsupported;
          request.fd = directory;
          request.path = path ? path : "";
          if (how) {
            request.extension_flags = how->flags;
            request.extension_mode = how->mode;
            request.extension_resolve = how->resolve;
          }
          return request;
        }()} {}

  Open2(const char* path, const open_how* how) : Open2{AT_FDCWD, path, how} {}

  expected<native_descriptor> await_resume() const noexcept {
    if (_user_data.result < 0)
      return std::unexpected{Error{static_cast<int>(-_user_data.result)}};
    return static_cast<int>(_user_data.result);
  }
};
}  // namespace faio::io::detail
#endif
