/**
 * @file faio/detail/common/error.hpp
 * @brief 错误值、错误域与诊断信息。
 */
#ifndef FAIO_DETAIL_COMMON_ERROR_HPP
#define FAIO_DETAIL_COMMON_ERROR_HPP

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif
#include <string_view>

namespace faio {
/** @brief 原生错误的来源；解析器错误不能作为 errno 解释。 */
enum class error_domain {
  faio,       /**< faio 自定义错误码。 */
  posix,      /**< errno 定义的系统调用错误。 */
  resolver,   /**< 地址解析器 getaddrinfo 返回的错误。 */
  win32,      /**< Windows 系统接口错误。 */
  winsock,    /**< Windows socket 接口错误。 */
  tls,        /**< TLS 配置、握手、证书验证或记录层错误。 */
  process = 7 /**< 子进程启动、等待、管道或捕获错误；错误域编号保持不变。 */
};

/** @brief 可选模块共用的诊断上下文，持有诊断数据；完整定义位于 Error 之后。 */
struct error_context;

/** @brief 错误值。 */
class Error {
 public:
  /** @brief 自定义错误码从 1000 开始，0 至 999 留给系统错误。 */
  enum ErrorCode {
    EmptySqe = 1000,   /**< io_uring 暂无可用的提交队列条目。 */
    InvalidAddresses,  /**< 没有可用于本次操作的有效地址。 */
    ClosedChannel,     /**< 通道已经关闭，不能继续传递数据。 */
    UnexpectedEOF,     /**< 尚未完成预期读取便遇到流末尾。 */
    WriteZero,         /**< 仍有待写数据时，底层写入返回零字节。 */
    TooLongTime,       /**< 时间参数超出支持范围。 */
    PassedTime,        /**< 请求的时间点已经过去。 */
    InvalidSocketType, /**< socket 类型与所请求的操作不匹配。 */
    ReuniteFailed,     /**< 尝试合并来自不同连接的读写半部。 */
  };

 public:
  /**
   * @brief 构造轻量错误值，保存错误域和已完成的进度。
   * @param err_code 原生错误码或相应错误域内的枚举值。
   * @param transferred 失败前已经生效的字节数。
   * @param domain 决定 err_code 解释方式的错误来源。
   */
  explicit Error(int err_code,
                 std::uint64_t transferred = 0,
                 error_domain domain = error_domain::posix)
      : err_code_{err_code}, transferred_(transferred), domain_(domain) {}

  /** @brief 失败前已经生效的字节数；取消不回滚系统调用的副作用。 */
  std::uint64_t transferred() const noexcept { return transferred_; }

  /** @brief 取得已完成进度，与 transferred 返回相同的字节数。 */
  std::uint64_t progress() const noexcept { return transferred_; }

  /** @brief 取得错误来源；不同错误域中的相同数值可能表示不同含义。 */
  error_domain domain() const noexcept { return domain_; }

  /** @brief 以有符号宽整数返回原生错误码。 */
  std::int64_t native_code() const noexcept { return err_code_; }

  /**
   * @brief 取得只读共享诊断上下文；轻量错误可能没有上下文。
   * @return 由本错误持有的共享指针引用，借用期间须保持错误对象有效。
   */
  auto context() const noexcept -> const std::shared_ptr<const error_context>& { return context_; }

  /**
   * @brief 复制错误并让副本持有诊断上下文，原错误保持不变。
   * @param details 与错误副本共享的不可变诊断信息。
   * @return 保留错误码、错误域和进度的新错误。
   */
  auto with_context(std::shared_ptr<const error_context> details) const -> Error {
    auto result = *this;
    result.context_ = std::move(details);
    return result;
  }

  /**
   * @brief 复制错误并更新进度，保留原错误域与完整诊断上下文。
   * @param bytes 新的已完成字节数，包含调用者需要累计的历史进度。
   * @return 更新进度后的错误副本。
   */
  auto with_progress(std::uint64_t bytes) const -> Error {
    auto result = *this;
    result.transferred_ = bytes;
    return result;
  }

 public:
  /** @brief 取得错误域内的原始整数码。 */
  [[nodiscard]]
  auto value() const noexcept -> int {
    return err_code_;
  }

  /**
   * @brief 优先返回持有的诊断消息；没有诊断时，按错误域生成可读消息。
   * @return 借用的消息视图；有上下文时其有效期受上下文生命周期约束。
   */
  [[nodiscard]]
  auto message() const noexcept -> std::string_view;

 private:
  int err_code_;                /**< 原生错误码，与错误域一起解释系统调用失败。 */
  std::uint64_t transferred_{}; /**< 操作已完成的字节数，失败或取消时仍保留有效进度。 */
  error_domain domain_{};       /**< 错误码所属的系统错误域。 */
  std::shared_ptr<const error_context> context_{}; /**< 可选的不可变诊断，复制错误时共享所有权。 */
};

/** @brief 持有失败诊断的数据；基础头不依赖可选组件。 */
struct error_context {
  std::string message;                       /**< 持有可读诊断文本，避免借用临时错误字符串。 */
  std::vector<std::uint64_t> native_reasons; /**< TLS 等后端提供的原生原因码，按提取顺序保存。 */
  std::optional<std::int64_t> verify_code;   /**< 可选的证书验证结果码。 */
  std::optional<Error> cause;              /**< 引发本层失败的底层错误，保留其域、进度和上下文。 */
  std::optional<std::uint32_t> process_id; /**< 与进程失败关联的原生子进程标识。 */
  std::optional<std::uint32_t> process_exit_code; /**< 已观察到的原生进程退出码。 */
  std::optional<int> process_signal;              /**< POSIX 子进程因信号退出时的信号编号。 */
  std::vector<char> captured_stdout;              /**< 捕获失败前已保留的标准输出，可包含零字节。 */
  std::vector<char> captured_stderr;              /**< 捕获失败前已保留的标准错误，可包含零字节。 */
  bool stdout_truncated{};                        /**< 标准输出是否超过保留上限而发生截断。 */
  bool stderr_truncated{};                        /**< 标准错误是否超过保留上限而发生截断。 */
};

/** @brief 依次从诊断上下文、模块错误域和系统错误码获取错误消息。 */
inline auto Error::message() const noexcept -> std::string_view {
  if (context_)
    return context_->message;
  if (domain_ == error_domain::tls)
    return "TLS error";
  if (domain_ == error_domain::process)
    return "Process error";
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

/**
 * @brief 用默认 POSIX 错误域创建不含额外上下文的错误值。
 * @param err errno 或 faio 自定义错误码。
 * @return 进度为零的错误对象。
 */
[[nodiscard]]
static inline auto make_error(int err) -> Error {
  return Error{err};
}

/**
 * @brief faio 异步接口使用的值或 Error 结果类型。
 * @tparam T 成功值类型，可以为 void。
 */
template <typename T>
using expected = std::expected<T, Error>;
}  // namespace faio

namespace std {
/** @brief 允许通过标准格式化接口输出 faio 错误的消息和数值。 */
template <>
class formatter<faio::Error> {
 public:
  /**
   * @brief 校验错误对象的格式说明符并返回解析停止位置。
   * @param context 标准格式串解析上下文。
   * @return 当前格式项结束处的迭代器。
   */
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

  /**
   * @brief 将错误消息与整数错误码写入格式化输出。
   * @param error 待展示的错误值。
   * @param context 持有输出迭代器的格式化上下文。
   * @return 写入结束位置的迭代器。
   */
  auto format(const faio::Error& error, auto& context) const noexcept {
    return format_to(context.out(), "{} (error {})", error.message(), error.value());
  }
};
}  // namespace std
#endif
