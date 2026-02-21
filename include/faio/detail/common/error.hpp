#ifndef FAIO_DETAIL_COMMON_ERROR_HPP
#define FAIO_DETAIL_COMMON_ERROR_HPP

#include <cassert>
#include <cstring>
#include <expected>
#include <format>
#include <string_view>

namespace faio {

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
    // HTTP/2 errors
    Http2Protocol = 2000,
    Http2ExpectedPreface,  // 客户端未发 HTTP/2 连接前言（例如浏览器发的是 HTTP/1.1）
    Http2StreamClosed,
    Http2StreamReset,
    Http2Refused,
    Http2Internal,
    Http2FlowControl,
    Http2SettingsTimeout,
    Http2PushPromiseRefused,
    Http2AuthenticationRequired,
  };

public:
  explicit Error(int err_code) : err_code_{err_code} {}

public:
  [[nodiscard]]
  auto value() const noexcept -> int {
    return err_code_;
  }

  // 是
  [[nodiscard]]
  auto message() const noexcept -> std::string_view {
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
    case Http2Protocol:
      return "HTTP/2 protocol error";
    case Http2ExpectedPreface:
      return "Expected HTTP/2 connection preface; client may be using HTTP/1.1 (e.g. browser)";
    case Http2StreamClosed:
      return "HTTP/2 stream closed";
    case Http2StreamReset:
      return "HTTP/2 stream reset";
    case Http2Refused:
      return "HTTP/2 stream refused";
    case Http2Internal:
      return "HTTP/2 internal error";
    case Http2FlowControl:
      return "HTTP/2 flow control error";
    case Http2SettingsTimeout:
      return "HTTP/2 settings timeout";
    case Http2PushPromiseRefused:
      return "HTTP/2 push promise refused";
    case Http2AuthenticationRequired:
      return "HTTP/2 authentication required";
    default:
      return strerror(err_code_);
    }
  }

private:
  int err_code_;
};

[[nodiscard]]
static inline auto make_error(int err) -> Error {
  return Error{err};
}

template <typename T> using expected = std::expected<T, Error>;

} // namespace faio

namespace std {

template <> class formatter<faio::Error> {
public:
  constexpr auto parse(format_parse_context &context) {
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

  auto format(const faio::Error &error, auto &context) const noexcept {
    return format_to(context.out(), "{} (error {})", error.message(),
                     error.value());
  }
};

} // namespace std
#endif // FAIO_DETAIL_COMMON_ERROR_HPP