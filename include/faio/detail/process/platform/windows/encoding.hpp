/**
 * @file faio/detail/process/platform/windows/encoding.hpp
 * @brief Windows 环境键比较、UTF-16 转换与 CRT 参数引用。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/process/types.hpp"
#include "faio/detail/io/platform/windows/error.hpp"
#include <algorithm>
#include <climits>
#include <string>
#include <string_view>

namespace faio::process::detail {
/** @brief 严格将 UTF-8 转为 UTF-16，拒绝无法表示的原生调用长度。 */
inline auto widen(std::string_view value) -> expected<std::wstring> {
  if (value.empty())
    return std::wstring{};
  if (value.size() > INT_MAX)
    return std::unexpected{process_error(errc::INVALID_CONFIGURATION)};
  const int size = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!size)
    return std::unexpected{process_error(errc::INVALID_CONFIGURATION)};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (!::MultiByteToWideChar(CP_UTF8,
                             MB_ERR_INVALID_CHARS,
                             value.data(),
                             static_cast<int>(value.size()),
                             result.data(),
                             size))
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  return result;
}

/** @brief 按 Windows 不区分大小写的序数规则比较原生环境键。 */
inline bool native_key_equal(std::wstring_view left, std::wstring_view right) {
  return ::CompareStringOrdinal(left.data(),
                                static_cast<int>(left.size()),
                                right.data(),
                                static_cast<int>(right.size()),
                                TRUE)
         == CSTR_EQUAL;
}

/** @brief 使用同一 Windows 序数规则对原生环境键排序。 */
inline bool native_key_less(std::wstring_view left, std::wstring_view right) {
  return ::CompareStringOrdinal(left.data(),
                                static_cast<int>(left.size()),
                                right.data(),
                                static_cast<int>(right.size()),
                                TRUE)
         == CSTR_LESS_THAN;
}

/** @brief 按 Windows CRT 规则为单个参数添加引号，保留原有引号和末尾反斜线。 */
inline std::wstring quote_argument(std::wstring_view value) {
  if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
    return std::wstring{value};
  std::wstring result{L"\""};
  std::size_t slashes{};
  for (wchar_t c : value) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    if (c == L'"')
      result.append(slashes * 2 + 1, L'\\');
    else
      result.append(slashes, L'\\');
    slashes = 0;
    result.push_back(c);
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}
}  // namespace faio::process::detail
#endif
