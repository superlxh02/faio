/**
 * @file faio/detail/process/types.hpp
 * @brief 进程错误码、退出状态与有界输出捕获选项。
 */
#pragma once
#include "faio/detail/common/error.hpp"
#include "faio/detail/io/stream/operation_options.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace faio::process {
/** @brief 用于跨平台保存非零进程标识的无符号整数类型。 */
using process_id = std::uint32_t;
/** @brief 进程域的稳定错误编号，原生系统错误保留原有错误域。 */
enum class errc : int {
  INVALID_CONFIGURATION = 1, /**< 参数、路径、环境或平台配置不满足启动约定。 */
  RUNTIME_UNAVAILABLE,       /**< 调用时没有可用 IO 上下文。 */
  PROCESS_LIMIT_REACHED,     /**< 程序共享服务的活进程记录表已满。 */
  LAUNCH_QUEUE_FULL,         /**< 程序共享服务的有界启动队列已满。 */
  RUNTIME_SHUTDOWN,          /**< 原 IO 域已经停止，不再接纳本次操作。 */
  OPERATION_IN_PROGRESS,     /**< 该进程已有独占退出等待或结果尚未完成。 */
  OUTPUT_LIMIT_EXCEEDED,     /**< 至少一个输出超过独立预算或双流合计预算。 */
  CHILD_NOT_WAITABLE,        /**< 记录对应的子进程已被外部回收，无法再取得退出状态。 */
  UNSUPPORTED_STDIO,         /**< 请求的标准流不是支持的普通文件或管道。 */
  UNSUPPORTED_CREATION_FLAGS /**< Windows 创建标志超出允许集合。 */
};

/** @brief 一次显式环境变量赋值所需的键和值。 */
struct environment_variable {
  std::string key;   /**< 环境变量键；显式配置须为合法 UTF-8 且不含等号或 NUL。 */
  std::string value; /**< 独立持有的 UTF-8 环境变量值，可以为空字符串。 */
};

/** @brief 命令环境的单项覆盖或删除操作。 */
struct environment_change {
  std::string key;                  /**< 环境变量键；显式配置须为合法 UTF-8 且不含等号或 NUL。 */
  std::optional<std::string> value; /**< 有值表示覆盖，无值表示删除该键。 */
};

/** @brief 双输出独立预算、合计预算及整个捕获操作的取消选项。 */
struct capture_options {
  std::size_t max_stdout_bytes{16 * 1024
                               * 1024}; /**< stdout 最多保留的字节数，零允许仅探测 EOF。 */
  std::size_t max_stderr_bytes{16 * 1024
                               * 1024}; /**< stderr 最多保留的字节数，与 stdout 独立限制。 */
  std::size_t max_total_bytes{32 * 1024 * 1024}; /**< 两个输出共享的总保留字节上限。 */
  std::size_t read_chunk_bytes{32
                               * 1024}; /**< 每个读取分支的暂存容量，合法范围为一字节至一 MiB。 */
  io::operation_options operation{};    /**< 覆盖整个启动、等待和捕获过程的截止时间与取消选项。 */
};

namespace detail {
/** @brief 构造保留稳定编号的进程域错误。 */
inline error process_error(errc code) {
  return error{static_cast<int>(code), 0, error_domain::PROCESS};
}

/** @brief 验证单次读取块大小，零预算仍允许仅探测 EOF。 */
inline expected<void> validate_capture(const capture_options& options) {
  if (!options.read_chunk_bytes || options.read_chunk_bytes > 1024 * 1024)
    return std::unexpected{process_error(errc::INVALID_CONFIGURATION)};
  return {};
}
}  // namespace detail

/** @brief 已确认的退出终态，保留平台原始状态而不持有原生进程。 */
class exit_status {
 public:
  /** @brief 禁止构造尚无原始终态的退出状态。 */
  exit_status() = delete;
  /** @brief 复制平台原始退出状态的完整值。 */
  exit_status(const exit_status&) noexcept = default;
  /** @brief 复制平台原始退出状态的完整值。 */
  auto operator=(const exit_status&) noexcept -> exit_status& = default;
  /** @brief 复制原始退出状态；整数值移动后两者保持相同终态。 */
  exit_status(exit_status&&) noexcept = default;
  /** @brief 赋值原始退出状态；这是纯值操作，不持有进程资源。 */
  auto operator=(exit_status&&) noexcept -> exit_status& = default;
  /** @brief 结束退出状态值的生命周期，无需回收进程。 */
  ~exit_status() = default;

  /** @brief 判断子进程是否以退出码零正常结束。 */
  auto success() const noexcept -> bool { return code() == 0; }

  /** @brief 返回正常退出码；Unix 信号终止时为空。 */
  auto code() const noexcept -> std::optional<std::uint32_t> {
#if defined(_WIN32)
    return raw_;
#else
    if (WIFEXITED(raw_))
      return static_cast<std::uint32_t>(WEXITSTATUS(raw_));
    return {};
#endif
  }

  /** @brief 生成便于阅读的退出码或信号说明。 */
  auto to_string() const -> std::string {
    if (const auto value = code())
      return "exit code: " + std::to_string(*value);
#if !defined(_WIN32)
    return "signal: " + std::to_string(*signal()) + (core_dumped() ? " (core dumped)" : "");
#else
    return "exit code: " + std::to_string(raw_);
#endif
  }

  /** @brief 比较完整退出终态值。 */
  auto operator==(const exit_status&) const noexcept -> bool = default;
#if defined(_WIN32)
  /** @brief 保存完整 DWORD 退出码，不截断高位。 */
  static auto from_raw_windows(std::uint32_t code) noexcept -> exit_status {
    return exit_status{code};
  }

  /** @brief 返回未经截断的 Windows 原始退出码。 */
  auto raw_windows() const noexcept -> std::uint32_t { return raw_; }
#else
  /** @brief 只接受已退出或被信号终止的 waitpid 状态。 */
  static auto from_raw_unix(int status) -> expected<exit_status> {
    if (!WIFEXITED(status) && !WIFSIGNALED(status))
      return std::unexpected{detail::process_error(errc::INVALID_CONFIGURATION)};
    return exit_status{status};
  }

  /** @brief 返回最初的 waitpid 原始状态。 */
  auto raw_unix() const noexcept -> int { return raw_; }

  /** @brief 返回导致 Unix 子进程终止的信号；正常退出时为空。 */
  auto signal() const noexcept -> std::optional<int> {
    return WIFSIGNALED(raw_) ? std::optional{WTERMSIG(raw_)} : std::nullopt;
  }

  /** @brief 查询原始 Unix 状态是否带有核心转储标记。 */
  auto core_dumped() const noexcept -> bool {
#ifdef WCOREDUMP
    return WIFSIGNALED(raw_) && WCOREDUMP(raw_);
#else
    return false;
#endif
  }
#endif
 private:
#if defined(_WIN32)
  /** @brief 保存已经验证的原生退出终态。 */
  explicit exit_status(std::uint32_t raw) noexcept : raw_(raw) {}

  std::uint32_t raw_;
#else
  /** @brief 保存已经验证的原生退出终态。 */
  explicit exit_status(int raw) noexcept : raw_(raw) {}

  int raw_;
#endif
};

/** @brief 完整捕获成功后交付的退出状态与两个输出字节序列。 */
struct output {
  exit_status status;            /**< 已确认的子进程退出状态。 */
  std::vector<char> stdout_data; /**< 按读取顺序保存并持有标准输出字节，可包含 NUL。 */
  std::vector<char> stderr_data; /**< 按读取顺序保存并持有标准错误字节，可包含 NUL。 */
};
}  // namespace faio::process
