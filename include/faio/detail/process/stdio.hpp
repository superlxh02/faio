/**
 * @file faio/detail/process/stdio.hpp
 * @brief 描述子进程标准输入、标准输出和标准错误的绑定方式。
 */
#pragma once
#include <filesystem>
#include <optional>
#include <utility>

namespace faio::process {
/** @brief 标准流的继承、空设备、管道或普通文件绑定类别。 */
enum class StdioKind {
  inherit, /**< 继承父进程对应的标准流。 */
  null,    /**< 连接到平台空设备。 */
  piped,   /**< 建立父子进程之间的单向管道。 */
  file     /**< 按指定模式绑定普通文件。 */
};
/** @brief 普通文件作为标准流时的读取、截断或追加方式。 */
enum class StdioFileMode {
  read,     /**< 只读打开，供子进程标准输入使用。 */
  truncate, /**< 创建或截断普通文件，供标准输出或标准错误使用。 */
  append    /**< 保留原有内容并在文件末尾追加输出。 */
};

/** @brief 拥有文件路径的标准流配置值，移动后恢复为继承绑定。 */
class Stdio {
 public:
  /** @brief 构造标准流绑定；默认继承父进程对应标准流。 */
  Stdio() noexcept = default;
  /** @brief 复制标准流绑定配置的完整值。 */
  Stdio(const Stdio&) = default;
  /** @brief 复制标准流绑定配置的完整值。 */
  auto operator=(const Stdio&) -> Stdio& = default;

  /** @brief 转交绑定类别、路径及模式，源对象恢复为继承绑定。 */
  Stdio(Stdio&& other) noexcept
      : kind_(std::exchange(other.kind_, StdioKind::inherit)),
        path_(std::move(other.path_)),
        mode_(std::exchange(other.mode_, {})) {
    other.path_.clear();
  }

  /** @brief 替换绑定类别、路径及模式，源对象恢复为继承绑定。 */
  auto operator=(Stdio&& other) noexcept -> Stdio& {
    if (this != &other) {
      kind_ = std::exchange(other.kind_, StdioKind::inherit);
      path_ = std::move(other.path_);
      mode_ = std::exchange(other.mode_, {});
      other.path_.clear();
    }
    return *this;
  }

  /** @brief 释放拥有的文件路径，不执行文件或管道 IO。 */
  ~Stdio() = default;

  /** @brief 构造继承父进程对应标准流的配置。 */
  static auto inherit() noexcept -> Stdio { return {}; }

  /** @brief 构造连接到平台空设备的标准流配置。 */
  static auto null() noexcept -> Stdio {
    Stdio result;
    result.kind_ = StdioKind::null;
    return result;
  }

  /** @brief 构造父子进程之间的单向管道配置。 */
  static auto piped() noexcept -> Stdio {
    Stdio result;
    result.kind_ = StdioKind::piped;
    return result;
  }

  /**
   * @brief 保存普通文件绑定路径及打开模式，不在此时打开文件。
   * @param path 相对路径以快照解析的子进程工作目录为基准。
   * @param mode stdin 使用 read，stdout 和 stderr 使用 truncate 或 append。
   */
  static auto from_file(std::filesystem::path path, StdioFileMode mode) -> Stdio {
    Stdio result;
    result.kind_ = StdioKind::file;
    result.path_ = std::move(path);
    result.mode_ = mode;
    return result;
  }

  /** @brief 返回当前标准流绑定类别。 */
  auto kind() const noexcept -> StdioKind { return kind_; }

  /** @brief 借用配置拥有的文件路径，仅在文件绑定时有意义。 */
  auto file_path() const noexcept -> const std::filesystem::path& { return path_; }

  /** @brief 返回文件绑定模式，其他绑定类别为空。 */
  auto file_mode() const noexcept -> std::optional<StdioFileMode> { return mode_; }

 private:
  StdioKind kind_{StdioKind::inherit}; /**< 标准流绑定类别，默认继承父进程。 */
  std::filesystem::path path_;         /**< 文件绑定拥有的原生路径，其余类别保持空。 */
  std::optional<StdioFileMode> mode_;  /**< 文件绑定的打开模式，其余类别无值。 */
};
}  // namespace faio::process
