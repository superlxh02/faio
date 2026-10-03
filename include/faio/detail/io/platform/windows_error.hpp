#pragma once
/** @file windows_error.hpp @brief 保留 Win32/Winsock 原生错误来源的 IO 编码桥。
 */
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/platform/windows_handle.hpp"
#include <cstdint>
#include <string>

namespace faio::io::windows {
/** @brief 负完成结果的内部标签；不侵入公共 Error 和通用完成协议。 */
inline constexpr int win32_error_tag = 0x10000000;
inline constexpr int winsock_error_tag = 0x20000000;
inline constexpr int native_error_mask = 0x0fffffff;

/** @brief 将原生 Win32 错误放进正整数，后端再按统一协议取负。 */
inline int encode_windows_error(std::uint32_t value) noexcept {
  return win32_error_tag | static_cast<int>(value & native_error_mask);
}

/** @brief Winsock 与 Win32 数值空间独立，同一整数不会丢失错误来源。 */
inline int encode_winsock_error(int value) noexcept {
  return winsock_error_tag | (value & native_error_mask);
}

/** @brief 文件服务路径直接交付原生 Win32 代码，无需内部编码中转。 */
inline Error make_windows_error(std::uint32_t value, std::uint64_t progress = 0) noexcept {
  return Error{static_cast<int>(value), progress, error_domain::win32};
}

/** @brief 网络同步控制面返回原生 Winsock 错误。 */
inline Error make_winsock_error(int value, std::uint64_t progress = 0) noexcept {
  return Error{value, progress, error_domain::winsock};
}

/** @brief 在 IO await_resume 边界解码；取消/超时仍保留普通 errno 契约。 */
inline Error make_io_error(int value, std::uint64_t progress = 0) noexcept {
  if ((value & 0xf0000000) == win32_error_tag)
    return make_windows_error(static_cast<std::uint32_t>(value & native_error_mask), progress);
  if ((value & 0xf0000000) == winsock_error_tag)
    return make_winsock_error(value & native_error_mask, progress);
  return Error{value, progress};
}

/** @brief 按 Error 的真实来源格式化 UTF-8 文字，便于 Windows IO 日志和诊断。
 * @param error 保留原生代码/来源的 IO 错误；不会把 Win32 代码当作 errno。
 * @return 拥有型 UTF-8 字符串，系统无法查询消息时仍包含来源和十进制原生代码。
 * @details FormatMessageW 使用系统语言，显式转成 UTF-8，不经过当前 ANSI
 *          代码页。该接口位于 Windows IO 层，通用 Error
 * 类型无需依赖系统缓冲区。
 */
inline std::string format_windows_error(const Error& error) {
  if (error.domain() != error_domain::win32 && error.domain() != error_domain::winsock)
    return std::string{error.message()};
  wchar_t* message{};
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      static_cast<DWORD>(error.native_code()),
      0,
      reinterpret_cast<wchar_t*>(&message),
      0,
      nullptr);

  struct release_buffer {
    wchar_t* value{};

    ~release_buffer() {
      if (value)
        ::LocalFree(value);
    }  // 转换/分配异常也释放系统拥有内存。
  } owner{message};

  if (length) {
    std::size_t trimmed = length;
    while (trimmed
           && (message[trimmed - 1] == L'\r' || message[trimmed - 1] == L'\n'
               || message[trimmed - 1] == L' '))
      --trimmed;  // 去掉系统消息尾部换行，使一条 IO 错误只占一行日志。
    if (trimmed) {
      const int bytes = ::WideCharToMultiByte(CP_UTF8,
                                              WC_ERR_INVALID_CHARS,
                                              message,
                                              static_cast<int>(trimmed),
                                              nullptr,
                                              0,
                                              nullptr,
                                              nullptr);
      if (bytes > 0) {
        std::string result(static_cast<std::size_t>(bytes), '\0');
        if (::WideCharToMultiByte(CP_UTF8,
                                  WC_ERR_INVALID_CHARS,
                                  message,
                                  static_cast<int>(trimmed),
                                  result.data(),
                                  bytes,
                                  nullptr,
                                  nullptr))
          return result;
      }
    }
  }
  return std::string{error.domain() == error_domain::winsock ? "Winsock error " : "Win32 error "}
         + std::to_string(error.native_code());  // 未知厂商/文件系统错误仍可按原生值定位。
}
}  // namespace faio::io::windows
