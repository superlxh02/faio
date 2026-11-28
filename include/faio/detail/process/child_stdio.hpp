/**
 * @file faio/detail/process/child_stdio.hpp
 * @brief 子进程管道状态及读写方向的公共包装。
 */
#pragma once
#include "faio/detail/process/types.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/base/buffer.hpp"
#include "faio/detail/io/core/domain.hpp"
#include "faio/detail/io/stream/operation_options.hpp"
#include <atomic>
#include <memory>
#include <span>

namespace faio::process::detail {
/** @brief 管道资源状态，在包装对象移动和任务挂起期间地址保持不变。 */
class pipe_state {
 public:
  /** @brief 禁止复制稳定管道状态，避免重复持有所有权或复制运行状态。 */
  pipe_state(const pipe_state&) = delete;
  /** @brief 稳定管道状态不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const pipe_state&) -> pipe_state& = delete;
  /** @brief 稳定管道状态不能移动；回调和协程依赖其固定地址。 */
  pipe_state(pipe_state&&) = delete;
  /** @brief 稳定管道状态不支持移动赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(pipe_state&&) -> pipe_state& = delete;

  /** @brief 发起幂等关闭，后续资源排空仍由 IO 域完成。 */
  ~pipe_state() { request_close(); }

  /** @brief 先分配地址固定的状态，再将原生管道所有权交给 I/O 域。 */
  static auto adopt(io::io_context, io::detail::native_descriptor, bool)
      -> expected<std::shared_ptr<pipe_state>>;

  /** @brief 借用 IO 域拥有的管道资源状态。 */
  auto resource() const noexcept -> const io::detail::resource_ptr& { return resource_; }

  /** @brief 查询父端是否为管道写端。 */
  auto is_writer() const noexcept -> bool { return writer_; }

  /** @brief 查询 IO 域资源是否仍有效且尚未发起关闭。 */
  auto is_open() const noexcept -> bool {
    return resource_ && resource_->fd() >= 0 && !closing_.load();
  }

  /** @brief 只发起一次资源关闭请求，异步完成仍由 IO 域排空。 */
  void request_close() noexcept {
    if (!closing_.exchange(true) && resource_ && resource_->owner)
      resource_->owner->request_close(resource_);
  }

  /** @brief 先请求关闭，再等待 IO 域排空；取消不撤销已接纳的关闭责任。 */
  auto close(io::operation_options) -> task<expected<void>>;

 private:
  /** @brief 保存 I/O 域资源和父端方向，将关闭状态初始化为尚未请求。 */
  pipe_state(io::detail::resource_ptr resource, bool writer)
      : resource_(std::move(resource)), writer_(writer) {}

  io::detail::resource_ptr resource_; /**< IO 域接管的原生管道资源。 */
  bool writer_;                       /**< 父端为写端时为真，读端时为假。 */
  std::atomic<bool> closing_{};       /**< 确保关闭请求只向 IO 域提交一次。 */
};

/** @brief 验证管道状态，合并父级与显式取消请求，再等待底层读取排空。 */
auto pipe_read(std::shared_ptr<pipe_state>, std::span<char>, io::operation_options)
    -> task<expected<std::size_t>>;
/** @brief 验证管道状态，合并父级与显式取消请求，再等待底层写入排空。 */
auto pipe_write(std::shared_ptr<pipe_state>, std::span<const char>, io::operation_options)
    -> task<expected<std::size_t>>;
/** @brief 持有 io_buffer 到读取结束，并按实际字节数更新有效长度。 */
auto pipe_read_owned(std::shared_ptr<pipe_state>, io::io_buffer, io::operation_options)
    -> task<expected<io::io_transfer>>;
/** @brief 只写入 io_buffer 的有效字节，同时保留其所有权。 */
auto pipe_write_owned(std::shared_ptr<pipe_state>, io::io_buffer, io::operation_options)
    -> task<expected<io::io_transfer>>;
/** @brief 将关闭请求交给地址固定的管道状态处理；包装为空时返回 EBADF。 */
auto pipe_close(std::shared_ptr<pipe_state>, io::operation_options) -> task<expected<void>>;
/** @brief 管道无用户态缓存，仅检查有效性、取消及截止时间。 */
auto pipe_flush(std::shared_ptr<pipe_state>, io::operation_options) -> task<expected<void>>;
}  // namespace faio::process::detail

namespace faio::process {
/** @brief 父进程持有的子进程标准输入写端，析构会请求关闭。 */
class child_stdin {
 public:
  /** @brief 禁止构造尚未关联原生管道的写端。 */
  child_stdin() = delete;

  /** @brief 接管稳定的子进程标准输入写端状态。 */
  explicit child_stdin(std::shared_ptr<detail::pipe_state> state) : state_(std::move(state)) {}

  /** @brief 禁止复制子进程标准输入写端，避免重复持有所有权或复制运行状态。 */
  child_stdin(const child_stdin&) = delete;
  /** @brief 子进程标准输入写端不支持复制赋值，以免破坏已有所有权或关联关系。 */
  auto operator=(const child_stdin&) -> child_stdin& = delete;
  /** @brief 转交写端所有权，移动前创建的任务仍引用原状态。 */
  child_stdin(child_stdin&&) noexcept = default;

  /** @brief 先请求关闭原写端，再接管新写端状态。 */
  auto operator=(child_stdin&& other) noexcept -> child_stdin& {
    if (this != &other) {
      if (state_)
        state_->request_close();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  /** @brief 发起写端关闭，使剩余句柄释放后子进程可见 EOF。 */
  ~child_stdin() {
    if (state_)
      state_->request_close();
  }

  /**
   * @brief 等待一次管道写入，允许短写并通过 Error.progress 保留已提交进度。
   * @param bytes 保持有效至任务返回的借用字节区间，取消返回前也会排空请求。
   * @param options 本次写入的截止时间和显式取消令牌。
   * @return 本次实际写入字节数，可能小于 bytes.size()。
   */
  auto write(std::span<const char> bytes, io::operation_options options = {}) const
      -> task<expected<std::size_t>> {
    return detail::pipe_write(state_, bytes, options);
  }

  /**
   * @brief 持有缓冲区的所有权并等待一次写入，允许短写。
   * @param buffer 仅发送有效长度以内的字节，容量中的其余字节不会写出。
   * @param options 本次写入的截止时间和显式取消令牌。
   * @return 保留缓冲所有权的传输结果，或包含已完成进度的错误。
   */
  auto write(io::io_buffer buffer, io::operation_options options = {}) const
      -> task<expected<io::io_transfer>> {
    return detail::pipe_write_owned(state_, std::move(buffer), options);
  }

  /** @brief 检查管道及操作选项；管道没有需另行刷新的用户态缓存。 */
  auto flush(io::operation_options options = {}) const -> task<expected<void>> {
    return detail::pipe_flush(state_, options);
  }

  /** @brief 关闭标准输入写端，使子进程在已有字节之后读取到 EOF。 */
  auto shutdown(io::operation_options options = {}) const -> task<expected<void>> {
    return close(options);
  }

  /** @brief 请求关闭并等待 IO 域排空在途请求；包装移动后返回 EBADF。 */
  auto close(io::operation_options options = {}) const -> task<expected<void>> {
    return detail::pipe_close(state_, options);
  }

  /** @brief 查询管道是否仍接受新操作，移动后或关闭请求后为 false。 */
  auto is_open() const noexcept -> bool { return state_ && state_->is_open(); }

 private:
  /** @brief 允许 Child 在创建 wait 任务时固定仍持有的 stdin 状态。 */
  friend class child;
  std::shared_ptr<detail::pipe_state> state_; /**< 公共写端与已创建任务共享的稳定管道状态。 */
};

/** @brief 子进程标准输出的父读端；同一包装独占公共关闭责任。 */
class child_stdout {
 public:
  /** @brief 禁止构造未连接原生管道的读端。 */
  child_stdout() = delete;

  /** @brief 接管稳定管道状态，不复制原生资源。 */
  explicit child_stdout(std::shared_ptr<detail::pipe_state> state) : state_(std::move(state)) {}

  /** @brief 禁止复制读端，避免多个公共包装提前关闭同一资源。 */
  child_stdout(const child_stdout&) = delete;
  /** @brief 禁止复制赋值读端。 */
  auto operator=(const child_stdout&) -> child_stdout& = delete;
  /** @brief 转交读端所有权，移动前创建的任务仍引用原状态。 */
  child_stdout(child_stdout&&) noexcept = default;

  /** @brief 先请求关闭原读端，再转交新读端状态。 */
  auto operator=(child_stdout&& other) noexcept -> child_stdout& {
    if (this != &other) {
      if (state_)
        state_->request_close();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  /** @brief 请求关闭读端，在途 IO 由原 IO 域排空。 */
  ~child_stdout() {
    if (state_)
      state_->request_close();
  }

  /**
   * @brief 读取至多 bytes.size() 字节，返回零表示 EOF。
   * @param bytes 借用的缓冲区，须保持有效直到任务返回。
   * @param options 本次读取的截止时间和显式取消令牌。
   * @return 实际字节数或包含已完成进度的错误。
   */
  auto read(std::span<char> bytes, io::operation_options options = {}) const
      -> task<expected<std::size_t>> {
    return detail::pipe_read(state_, bytes, options);
  }

  /**
   * @brief 持有缓冲到读取完成，并返回有效长度已更新的 io_transfer。
   * @param buffer 可写容量限定本次最多读取的字节数。
   * @param options 本次读取的截止时间和显式取消令牌。
   * @return 仍持有缓冲的传输结果或错误。
   */
  auto read(io::io_buffer buffer, io::operation_options options = {}) const
      -> task<expected<io::io_transfer>> {
    return detail::pipe_read_owned(state_, std::move(buffer), options);
  }

  /** @brief 关闭读端并等待在途 IO 排空。 */
  auto close(io::operation_options options = {}) const -> task<expected<void>> {
    return detail::pipe_close(state_, options);
  }

  /** @brief 查询该读端是否仍接受新读取。 */
  auto is_open() const noexcept -> bool { return state_ && state_->is_open(); }

 private:
  std::shared_ptr<detail::pipe_state>
      state_; /**< 持有管道状态，直到公共包装和已创建任务释放其引用。 */
};

/** @brief 子进程标准错误的父读端；同一包装独占公共关闭责任。 */
class child_stderr {
 public:
  /** @brief 禁止构造未连接原生管道的读端。 */
  child_stderr() = delete;

  /** @brief 接管稳定管道状态，不复制原生资源。 */
  explicit child_stderr(std::shared_ptr<detail::pipe_state> state) : state_(std::move(state)) {}

  /** @brief 禁止复制读端，避免多个公共包装提前关闭同一资源。 */
  child_stderr(const child_stderr&) = delete;
  /** @brief 禁止复制赋值读端。 */
  auto operator=(const child_stderr&) -> child_stderr& = delete;
  /** @brief 转交读端所有权，移动前创建的任务仍引用原状态。 */
  child_stderr(child_stderr&&) noexcept = default;

  /** @brief 先请求关闭原读端，再转交新读端状态。 */
  auto operator=(child_stderr&& other) noexcept -> child_stderr& {
    if (this != &other) {
      if (state_)
        state_->request_close();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  /** @brief 请求关闭读端，在途 IO 由原 IO 域排空。 */
  ~child_stderr() {
    if (state_)
      state_->request_close();
  }

  /**
   * @brief 读取至多 bytes.size() 字节，返回零表示 EOF。
   * @param bytes 借用的缓冲区，须保持有效直到任务返回。
   * @param options 本次读取的截止时间和显式取消令牌。
   * @return 实际字节数或包含已完成进度的错误。
   */
  auto read(std::span<char> bytes, io::operation_options options = {}) const
      -> task<expected<std::size_t>> {
    return detail::pipe_read(state_, bytes, options);
  }

  /**
   * @brief 持有缓冲到读取完成，并返回有效长度已更新的 io_transfer。
   * @param buffer 可写容量限定本次最多读取的字节数。
   * @param options 本次读取的截止时间和显式取消令牌。
   * @return 仍持有缓冲的传输结果或错误。
   */
  auto read(io::io_buffer buffer, io::operation_options options = {}) const
      -> task<expected<io::io_transfer>> {
    return detail::pipe_read_owned(state_, std::move(buffer), options);
  }

  /** @brief 关闭读端并等待在途 IO 排空。 */
  auto close(io::operation_options options = {}) const -> task<expected<void>> {
    return detail::pipe_close(state_, options);
  }

  /** @brief 查询该读端是否仍接受新读取。 */
  auto is_open() const noexcept -> bool { return state_ && state_->is_open(); }

 private:
  std::shared_ptr<detail::pipe_state>
      state_; /**< 持有管道状态，直到公共包装和已创建任务释放其引用。 */
};
}  // namespace faio::process
