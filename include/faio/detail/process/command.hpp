/**
 * @file faio/detail/process/command.hpp
 * @brief 可复制的启动配置及同步、异步启动接口。
 */
#pragma once
#include "faio/detail/process/stdio.hpp"
#include "faio/detail/process/types.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/core/context.hpp"
#include <filesystem>
#include <initializer_list>
#include <span>
#include <utility>

namespace faio::process {
/** @brief 独占公共控制权的子进程包装，原生退出记录由服务继续维护。 */
class child;

/** @brief 拥有启动参数的配置值；修改配置不改变已创建任务的快照。 */
class command {
 public:
  /** @brief 禁止省略程序路径，空路径也必须由调用方显式给出。 */
  command() = delete;

  /** @brief 保存程序路径；其余参数和标准流采用默认配置。 */
  explicit command(std::filesystem::path program) : program_(std::move(program)) {}

  /** @brief 复制完整的命令配置。 */
  command(const command&) = default;
  /** @brief 复制完整的命令配置。 */
  auto operator=(const command&) -> command& = default;

  /** @brief 接管完整命令配置，源对象恢复为空程序的默认配置。 */
  command(command&& other) noexcept : command(std::filesystem::path{}) { swap(other); }

  /** @brief 用移动源替换完整命令配置，源对象恢复为空程序的默认配置。 */
  auto operator=(command&& other) noexcept -> command& {
    if (this != &other) {
      command moved{std::move(other)};
      swap(moved);
    }
    return *this;
  }

  /** @brief 释放命令配置，不影响此前任务拥有的快照。 */
  ~command() = default;

  /** @brief 按顺序追加一个独立持有的 UTF-8 参数，不通过 shell 拼接。 */
  auto arg(std::string value) -> command& {
    args_.push_back(std::move(value));
    return *this;
  }

  /** @brief 按输入顺序复制追加一组参数。 */
  auto args(std::span<const std::string> values) -> command& {
    for (const auto& value : values)
      arg(value);
    return *this;
  }

  /** @brief 按输入顺序复制追加一组参数。 */
  auto args(std::initializer_list<std::string_view> values) -> command& {
    for (auto value : values)
      arg(std::string{value});
    return *this;
  }

  /** @brief 清除参数列表，保留程序及其他启动配置。 */
  auto args_clear() noexcept -> command& {
    args_.clear();
    return *this;
  }

  /** @brief 覆盖指定环境键的值，重复键按平台键比较规则替换。 */
  auto env(std::string key, std::string value) -> command& {
    set_env(std::move(key), std::move(value));
    return *this;
  }

  /** @brief 按顺序合并一组显式设置的环境变量覆盖项。 */
  auto envs(std::span<const environment_variable> values) -> command& {
    for (const auto& value : values)
      env(value.key, value.value);
    return *this;
  }

  /** @brief 记录删除该环境键的请求，应用于后续启动快照。 */
  auto env_remove(std::string key) -> command& {
    set_env(std::move(key), {});
    return *this;
  }

  /** @brief 取消继承父环境，并清空已有显式修改。 */
  auto env_clear() noexcept -> command& {
    envs_.clear();
    env_clear_ = true;
    return *this;
  }

  /** @brief 设置子进程目录，实际绝对路径在任务创建时固定。 */
  auto current_dir(std::filesystem::path path) -> command& {
    cwd_ = std::move(path);
    return *this;
  }

  /** @brief 恢复继承任务创建时父进程工作目录的策略。 */
  auto current_dir_clear() noexcept -> command& {
    cwd_.reset();
    return *this;
  }

  /** @brief 替换子进程标准输入绑定。 */
  auto set_stdin(stdio value) -> command& {
    stdin_ = std::move(value);
    return *this;
  }

  /** @brief 替换子进程标准输出绑定。 */
  auto set_stdout(stdio value) -> command& {
    stdout_ = std::move(value);
    return *this;
  }

  /** @brief 替换子进程标准错误绑定。 */
  auto set_stderr(stdio value) -> command& {
    stderr_ = std::move(value);
    return *this;
  }

  /** @brief 设置公共 Child 被丢弃时是否请求强制终止。 */
  auto kill_on_drop(bool enabled) noexcept -> command& {
    kill_on_drop_ = enabled;
    return *this;
  }

  /** @brief 借用配置拥有的程序路径。 */
  auto get_program() const noexcept -> const std::filesystem::path& { return program_; }

  /** @brief 借用当前参数序列，不包含自动生成的 argv[0]。 */
  auto get_args() const noexcept -> std::span<const std::string> { return args_; }

  /** @brief 借用环境修改序列，空 value 表示删除。 */
  auto get_envs() const noexcept -> std::span<const environment_change> { return envs_; }

  /** @brief 查询启动时是否禁用父环境继承。 */
  auto get_env_clear() const noexcept -> bool { return env_clear_; }

  /** @brief 借用显式子进程目录配置；为空时使用调用时目录。 */
  auto get_current_dir() const noexcept -> const std::optional<std::filesystem::path>& {
    return cwd_;
  }

  /** @brief 借用标准输入绑定配置。 */
  auto get_stdin() const noexcept -> const stdio& { return stdin_; }

  /** @brief 借用标准输出绑定配置。 */
  auto get_stdout() const noexcept -> const stdio& { return stdout_; }

  /** @brief 借用标准错误绑定配置。 */
  auto get_stderr() const noexcept -> const stdio& { return stderr_; }

  /** @brief 查询公共 Child 的析构终止策略。 */
  auto get_kill_on_drop() const noexcept -> bool { return kill_on_drop_; }

  /**
   * @brief 同步完成原生创建与服务接管，返回拥有标准流的 Child。
   * @param context 接管父端管道并关联关闭通知的 IO 上下文。
   * @return 拥有公共控制权的 Child；失败时不交付未接管进程。
   */
  auto spawn(io::io_context context = io::io_context::current()) const -> expected<child>;
  /**
   * @brief 立即固定启动快照，等待时交给有界启动服务执行。
   * @param options 整个异步操作的绝对截止时间及显式取消令牌。
   * @param context 任务创建时固定的 IO 上下文，不延后重新查询当前运行时。
   * @return 惰性任务；配置错误也由该任务返回，无需保存原 Command。
   */
  auto spawn_async(io::operation_options options = {},
                   io::io_context context = io::io_context::current()) const
      -> task<expected<child>>;
  /**
   * @brief 立即固定配置，异步启动并等待退出状态。
   * @param options 整个异步操作的绝对截止时间及显式取消令牌。
   * @param context 任务创建时固定的 IO 上下文，不延后重新查询当前运行时。
   * @return 惰性任务；配置错误也由该任务返回，无需保存原 Command。
   */
  auto status(io::operation_options options = {},
              io::io_context context = io::io_context::current()) const
      -> task<expected<exit_status>>;
  /**
   * @brief 立即固定配置，异步启动并有界并发捕获 stdout 与 stderr。
   * @param options 两路输出各自的资源上限及合计上限，以及整个操作的截止时间和取消令牌。
   * @param context 创建快照时固定的 IO 上下文。
   * @return 完整输出；失败时 Error 上下文保留已经接纳的输出前缀。
   */
  auto output(capture_options options = {},
              io::io_context context = io::io_context::current()) const
      -> task<expected<::faio::process::output>>;
#if defined(_WIN32)
  /** @brief 设置 Windows 创建标志，合法性由快照阶段统一验证。 */
  auto creation_flags(std::uint32_t flags) noexcept -> command& {
    flags_ = flags;
    return *this;
  }

  /** @brief 返回调用方设置的 Windows 创建标志。 */
  auto get_creation_flags() const noexcept -> std::uint32_t { return flags_; }
#else
  /** @brief 设置 Unix argv[0] 文本，不改变用于查找程序的路径。 */
  auto arg0(std::string value) -> command& {
    arg0_ = std::move(value);
    return *this;
  }

  /** @brief 借用 Unix argv[0] 覆盖值。 */
  auto get_arg0() const noexcept -> const std::optional<std::string>& { return arg0_; }

  /** @brief 设置 Unix 进程组；零表示为子进程创建新的进程组。 */
  auto process_group(std::int32_t group) noexcept -> command& {
    group_ = group;
    return *this;
  }

  /** @brief 返回可选 Unix 进程组设置。 */
  auto get_process_group() const noexcept -> std::optional<std::int32_t> { return group_; }
#endif
 private:
  /** @brief 在原位置替换相同环境键，避免同一键出现多项修改。 */
  void set_env(std::string key, std::optional<std::string> value);

  /** @brief 交换完整启动配置，保证移动后源对象恢复默认配置。 */
  void swap(command& other) noexcept {
    using std::swap;
    swap(program_, other.program_);
    swap(args_, other.args_);
    swap(envs_, other.envs_);
    swap(cwd_, other.cwd_);
    swap(stdin_, other.stdin_);
    swap(stdout_, other.stdout_);
    swap(stderr_, other.stderr_);
    swap(env_clear_, other.env_clear_);
    swap(kill_on_drop_, other.kill_on_drop_);
#if defined(_WIN32)
    swap(flags_, other.flags_);
#else
    swap(arg0_, other.arg0_);
    swap(group_, other.group_);
#endif
  }

  std::filesystem::path program_;            /**< 原始程序路径，快照阶段据此生成绝对候选路径。 */
  std::vector<std::string> args_;            /**< 不包含 argv[0] 的拥有型 UTF-8 参数序列。 */
  std::vector<environment_change> envs_;     /**< 按平台键规则去重的环境覆盖和删除序列。 */
  std::optional<std::filesystem::path> cwd_; /**< 可选子进程目录，相对值按调用时父目录解析。 */
  stdio stdin_;                              /**< 子进程标准输入绑定。 */
  stdio stdout_;                             /**< 子进程标准输出绑定。 */
  stdio stderr_;                             /**< 子进程标准错误绑定。 */
  bool env_clear_{};                         /**< 为真时不复制父进程环境。 */
  bool kill_on_drop_{};                      /**< 公共 Child 丢弃时是否请求强制终止。 */
#if defined(_WIN32)
  std::uint32_t flags_{};
#else
  std::optional<std::string> arg0_;
  std::optional<std::int32_t> group_;
#endif
};
}  // namespace faio::process
