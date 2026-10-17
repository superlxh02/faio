/**
 * @file faio/detail/io/platform/linux/open.hpp
 * @brief 跨平台异步 IO：打开文件。
 * @details 平台层负责原生句柄、系统调用与错误转换。稳定资源归属及完成责任由 IO 域协调，取消请求不能代替在途系统调用的安全完成。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <fcntl.h>
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

  /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
  expected<native_descriptor> await_resume() const noexcept {
    if (_user_data.result < 0)
      return std::unexpected{Error{static_cast<int>(-_user_data.result)}};
    return static_cast<int>(_user_data.result);
  }
};
}
