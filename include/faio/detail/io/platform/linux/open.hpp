/**
 * @file faio/detail/io/platform/linux/open.hpp
 * @brief 打开文件。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"
#include <fcntl.h>
#include <linux/openat2.h>

namespace faio::io::detail {
/**  @brief Linux openat2 扩展，按值持有 open_how 参数，磁盘调用交给文件服务。 */
class Open2 : public IORegistrantAwaiter<Open2> {
 public:
  /** @brief 构造带路径解析约束的文件打开等待器。 */
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

  /** @brief 构造带路径解析约束的文件打开等待器。 */
  Open2(const char* path, const open_how* how) : Open2{AT_FDCWD, path, how} {}

  /** @brief 取得异步操作结果。 */
  expected<native_descriptor> await_resume() const noexcept {
    if (_user_data.result < 0)
      return std::unexpected{Error{static_cast<int>(-_user_data.result)}};
    return static_cast<int>(_user_data.result);
  }
};
}  // namespace faio::io::detail
