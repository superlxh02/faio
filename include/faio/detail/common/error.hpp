#ifndef FAIO_DETAIL_COMMON_ERROR_HPP
#define FAIO_DETAIL_COMMON_ERROR_HPP

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif
#include <string_view>

namespace faio {
/// @brief 原生错误的来源；解析器错误不能作为 errno 解释。
enum class error_domain { faio, posix, resolver, win32, winsock };

// 错误类
class Error {
 public:
  // 自定义错误码，从1000开始，0-999留给系统
  enum ErrorCode {
    EmptySqe = 1000,
    InvalidAddresses,
    ClosedChannel,
    UnexpectedEOF,
    WriteZero,
    TooLongTime,
    PassedTime,
    InvalidSocketType,
    ReuniteFailed,
  };

 public:
  explicit Error(int err_code,
                 std::uint64_t transferred = 0,
                 error_domain domain = error_domain::posix)
      : err_code_{err_code}, transferred_(transferred), domain_(domain) {}

  /// @brief 失败前已经生效的字节数；取消不回滚系统调用的副作用。
  std::uint64_t transferred() const noexcept { return transferred_; }

  std::uint64_t progress() const noexcept { return transferred_; }

  error_domain domain() const noexcept { return domain_; }

  std::int64_t native_code() const noexcept { return err_code_; }

 public:
  [[nodiscard]]
  auto value() const noexcept -> int {
    return err_code_;
  }

  // 是
  [[nodiscard]]
  auto message() const noexcept -> std::string_view {
    if (domain_ == error_domain::resolver)
      return gai_strerror(err_code_);
    switch (err_code_) {
      case EmptySqe:
        return "No sqe is available";
      case InvalidAddresses:
        return "Invalid addresses";
      case ClosedChannel:
        return "Channel has closed";
      case UnexpectedEOF:
        return "Read EOF too early";
      case WriteZero:
        return "Write return zero";
      case TooLongTime:
        return "Time is too long";
      case PassedTime:
        return "Time has passed";
      case InvalidSocketType:
        return "Invalid socket type";
      case ReuniteFailed:
        return "Tried to reunite halves that are not from the same socket";
      default:
        return strerror(err_code_);
    }
  }

 private:
  int err_code_;
  std::uint64_t transferred_{};
  error_domain domain_{};
};

[[nodiscard]]
static inline auto make_error(int err) -> Error {
  return Error{err};
}

template <typename T>
using expected = std::expected<T, Error>;
}  // namespace faio

namespace std {
template <>
class formatter<faio::Error> {
 public:
  constexpr auto parse(format_parse_context& context) {
    auto it{context.begin()};
    auto end{context.end()};
    if (it == end || *it == '}') {
      return it;
    }
    ++it;
    if (it != end && *it != '}') {
      throw format_error("Invalid format specifier for Error");
    }
    return it;
  }

  auto format(const faio::Error& error, auto& context) const noexcept {
    return format_to(context.out(), "{} (error {})", error.message(), error.value());
  }
};
}  // namespace std
#endif  // FAIO_DETAIL_COMMON_ERROR_HPP
