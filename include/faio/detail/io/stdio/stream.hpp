/**
 * @file faio/detail/io/stdio/stream.hpp
 * @brief 标准流的协程接口；阻塞系统调用由辅助服务执行。
 */
#pragma once

#include "faio/detail/blocking/call.hpp"
#include "faio/detail/coroutine/context/this_coro.hpp"
#include "faio/detail/io/base/buffer.hpp"
#include "faio/detail/io/stream/operation.hpp"
#include "faio/detail/sync/mutex.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <concepts>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/error.hpp"
#include <io.h>
#else
#include "faio/detail/io/platform/unix/request_operations.hpp"
#include <unistd.h>
#endif

namespace faio::stdio {
namespace detail {
/** @brief 选择进程标准输出通道。 */
enum class output_channel { STANDARD_OUTPUT, STANDARD_ERROR };

/** @brief 输出调用共享逻辑锁，等待锁不占用辅助线程。 */
struct output_state {
  sync::mutex mutex;
};

/** @brief 输入流共享预读字节，临时流对象不会丢弃后续行。 */
struct input_state {
  sync::mutex mutex;
  std::array<char, 4096> buffer{};
  std::size_t begin{};
  std::size_t end{};
};

/** @brief 共享进程输入状态，操作按值保有状态直到安全完成。 */
inline auto shared_input() -> std::shared_ptr<input_state> {
  static auto state = std::make_shared<input_state>();
  return state;
}

/** @brief 分别串行标准输出与标准错误，不混用两者的记录锁。 */
inline auto shared_output(output_channel channel) -> std::shared_ptr<output_state> {
  static auto output = std::make_shared<output_state>();
  static auto errors = std::make_shared<output_state>();
  return channel == output_channel::STANDARD_OUTPUT ? output : errors;
}

/** @brief 借用标准 C 流，不关闭或替换它。 */
inline auto output_file(output_channel channel) noexcept -> std::FILE* {
  return channel == output_channel::STANDARD_OUTPUT ? stdout : stderr;
}

#if defined(F_SETNOSIGPIPE) && defined(F_GETNOSIGPIPE)
/** @brief Darwin 的管道信号由进程接收，临时使用原生禁止 SIGPIPE 标志。 */
class no_sigpipe_state {
 public:
  auto acquire(int descriptor) -> bool {
    const std::lock_guard guard{mutex_};
    for (std::size_t index = 0; index != count_; ++index) {
      if (entries_[index].descriptor == descriptor) {
        ++active_;
        return true;
      }
    }
    const auto previous = ::fcntl(descriptor, F_GETNOSIGPIPE);
    if (previous < 0 || ::fcntl(descriptor, F_SETNOSIGPIPE, 1) < 0)
      return false;
    entries_[count_++] = {descriptor, previous};
    ++active_;
    return true;
  }

  void release() {
    const std::lock_guard guard{mutex_};
    if (--active_)
      return;
    // 两个标准描述符可能来自同一个 dup；逆序恢复才能保留它们的原始共享标志。
    while (count_) {
      const auto& entry = entries_[--count_];
      (void)::fcntl(entry.descriptor, F_SETNOSIGPIPE, entry.previous);
    }
  }

 private:
  struct entry {
    int descriptor;
    int previous;
  };

  std::mutex mutex_;
  std::array<entry, 2> entries_{};
  std::size_t count_{};
  std::size_t active_{};
};

/** @brief 两个通道只在设置与恢复信号标志时共用短锁，系统写入仍然独立。 */
inline auto shared_no_sigpipe() -> no_sigpipe_state& {
  static no_sigpipe_state state;
  return state;
}
#endif

#if !defined(_WIN32)
/** @brief 将断管道转换为 EPIPE，不改变进程信号处理器或描述符的阻塞模式。 */
template <class Function>
auto output_call(output_channel channel, Function&& function) -> std::invoke_result_t<Function&> {
#if defined(F_SETNOSIGPIPE) && defined(F_GETNOSIGPIPE)
  auto& state = shared_no_sigpipe();
  if (!state.acquire(::fileno(output_file(channel))))
    return -1;

  struct release_guard {
    no_sigpipe_state& state;

    ~release_guard() { state.release(); }
  };

  int saved_error{};
  const auto result = [&] {
    const release_guard guard{state};
    const auto value = function();
    saved_error = errno;
    return value;
  }();
  errno = saved_error;
  return result;
#else
  (void)channel;
  return io::detail::unix_operations::pipe_write(std::forward<Function>(function));
#endif
}
#endif

/** @brief 将父级和显式取消统一转发给本次操作。 */
struct forward_stop {
  std::stop_source* target;

  void operator()() const noexcept { target->request_stop(); }
};

/** @brief 在逻辑锁保护下，把系统调用交给独立辅助服务。 */
template <class State, class Function>
auto locked_call(std::shared_ptr<State> state,
                 Function function,
                 std::optional<std::chrono::steady_clock::time_point> deadline)
    -> task<std::invoke_result_t<Function&>> {
  try {
    auto guard = co_await state->mutex.scoped_lock();
    auto operation = blocking::call(io::io_context::current(), std::move(function));
    if (deadline)
      operation.set_timeout_at(*deadline);
    co_return co_await std::move(operation);
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}

/**
 * @brief 合并停止选项；借用内存一直保留到已开始的阻塞调用结束。
 * @details 取消或期限不能强制中断已开始的原生 read/write；排空后才返回错误。
 */
template <class State, class Function>
auto perform(std::shared_ptr<State> state, Function function, io::operation_options options)
    -> task<std::invoke_result_t<Function&>> {
  if (options.deadline && std::chrono::steady_clock::now() >= *options.deadline)
    co_return std::unexpected{make_error(ETIMEDOUT)};
  std::stop_source stop;
  auto parent = co_await this_coro::stop_token();
  std::optional<std::stop_callback<forward_stop>> inherited{}, explicit_stop{};
  if (parent.stop_possible())
    inherited.emplace(parent, forward_stop{&stop});
  if (options.stop.stop_possible())
    explicit_stop.emplace(options.stop, forward_stop{&stop});
  co_return co_await io::detail::bind_stop(
      locked_call(std::move(state), std::move(function), options.deadline), stop.get_token());
}

/** @brief 返回首批可读字节，不等待填满调用方的整个缓冲区。 */
inline auto native_read(std::span<char> bytes) -> expected<std::size_t> {
  if (bytes.empty())
    return 0;
#if defined(_WIN32)
  int count{};
  do {
    count = ::_read(::_fileno(stdin),
                    bytes.data(),
                    static_cast<unsigned>(std::min<std::size_t>(bytes.size(), INT_MAX)));
  } while (count < 0 && errno == EINTR);
#else
  ssize_t count{};
  do {
    count = ::read(
        ::fileno(stdin), bytes.data(), std::min(bytes.size(), static_cast<std::size_t>(SSIZE_MAX)));
  } while (count < 0 && errno == EINTR);
#endif
  if (count < 0)
    return std::unexpected{make_error(errno)};
  return static_cast<std::size_t>(count);
}

/** @brief 先领取 read_line 预读的字节，再访问标准输入句柄。 */
inline auto read_locked(input_state& state, std::span<char> bytes) -> expected<std::size_t> {
  if (bytes.empty())
    return 0;
  if (state.begin != state.end) {
    const auto count = std::min(bytes.size(), state.end - state.begin);
    std::memcpy(bytes.data(), state.buffer.data() + state.begin, count);
    state.begin += count;
    return count;
  }
  return native_read(bytes);
}

/** @brief 在同一个输入锁内读取一行，并保留换行之后的预读字节。 */
inline auto read_line_locked(input_state& state,
                             std::optional<std::string>& line,
                             std::size_t max_bytes) -> expected<std::size_t> {
  std::string value;
  for (;;) {
    if (state.begin == state.end) {
      auto read = native_read(state.buffer);
      if (!read)
        return std::unexpected{read.error().with_progress(value.size())};
      state.begin = 0;
      state.end = *read;
      if (!*read) {
        if (value.empty()) {
          line.reset();
          return 0;
        }
        const auto size = value.size();
        line.emplace(std::move(value));
        return size;
      }
    }
    const auto available =
        std::string_view{state.buffer.data() + state.begin, state.end - state.begin};
    const auto newline = available.find('\n');
    const auto wanted = newline == std::string_view::npos ? available.size() : newline + 1;
    const auto count = std::min(wanted, max_bytes - value.size());
    value.append(available.data(), count);
    state.begin += count;
    if (count < wanted)
      return std::unexpected{::faio::error{EMSGSIZE, value.size()}};
    if (newline != std::string_view::npos) {
      const auto size = value.size();
      line.emplace(std::move(value));
      return size;
    }
    if (value.size() == max_bytes)
      return std::unexpected{::faio::error{EMSGSIZE, value.size()}};
  }
}

/** @brief 写出一批原始字节，保持标准流原有的阻塞模式。 */
inline auto native_write(output_channel channel, std::span<const char> bytes)
    -> expected<std::size_t> {
  if (bytes.empty())
    return 0;
#if defined(_WIN32)
  const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(output_file(channel))));
  if (handle == INVALID_HANDLE_VALUE)
    return std::unexpected{make_error(EBADF)};
  DWORD count{};
  if (!::WriteFile(handle,
                   bytes.data(),
                   static_cast<DWORD>(std::min<std::size_t>(bytes.size(), MAXDWORD)),
                   &count,
                   nullptr))
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  return static_cast<std::size_t>(count);
#else
  ssize_t count{};
  do {
    count = output_call(channel, [&] {
      return ::write(::fileno(output_file(channel)),
                     bytes.data(),
                     std::min(bytes.size(), static_cast<std::size_t>(SSIZE_MAX)));
    });
  } while (count < 0 && errno == EINTR);
  if (count < 0)
    return std::unexpected{make_error(errno)};
  return static_cast<std::size_t>(count);
#endif
}

/** @brief 完整写入一个记录，错误保留此前已完成的字节数。 */
inline auto write_all_locked(output_channel channel, std::span<const char> bytes)
    -> expected<std::size_t> {
  std::size_t total{};
  while (total != bytes.size()) {
    auto written = native_write(channel, bytes.subspan(total));
    if (!written)
      return std::unexpected{written.error().with_progress(total + written.error().progress())};
    if (!*written)
      return std::unexpected{::faio::error{::faio::error::WRITE_ZERO, total}};
    total += *written;
  }
  return total;
}

#if defined(_WIN32)
/** @brief 真实 Windows 控制台使用宽字符输出，重定向保持 UTF-8 原始字节。 */
inline auto write_text_locked(output_channel channel, std::string_view text)
    -> expected<std::size_t> {
  if (text.empty())
    return 0;
  const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(output_file(channel))));
  DWORD mode{};
  if (!::GetConsoleMode(handle, &mode))
    return write_all_locked(channel, {text.data(), text.size()});
  if (text.size() > static_cast<std::size_t>(INT_MAX))
    return std::unexpected{make_error(EOVERFLOW)};
  const auto length = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (!length)
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  if (!::MultiByteToWideChar(CP_UTF8,
                             MB_ERR_INVALID_CHARS,
                             text.data(),
                             static_cast<int>(text.size()),
                             wide.data(),
                             length))
    return std::unexpected{io::windows::make_windows_error(::GetLastError())};
  std::size_t total{};
  const auto progress = [&] {
    return total ? static_cast<std::size_t>(::WideCharToMultiByte(CP_UTF8,
                                                                  0,
                                                                  wide.data(),
                                                                  static_cast<int>(total),
                                                                  nullptr,
                                                                  0,
                                                                  nullptr,
                                                                  nullptr))
                 : std::size_t{};
  };
  while (total != wide.size()) {
    DWORD count{};
    auto remaining = std::min<std::size_t>(wide.size() - total, 32768);
    if (total + remaining != wide.size() && wide[total + remaining - 1] >= 0xd800
        && wide[total + remaining - 1] <= 0xdbff)
      --remaining;
    if (!::WriteConsoleW(
            handle, wide.data() + total, static_cast<DWORD>(remaining), &count, nullptr)) {
      const auto error = ::GetLastError();
      return std::unexpected{io::windows::make_windows_error(error, progress())};
    }
    if (!count)
      return std::unexpected{::faio::error{::faio::error::WRITE_ZERO, progress()}};
    total += count;
  }
  return text.size();
}
#else
/** @brief Unix 终端和重定向共享原始 UTF-8 字节输出。 */
inline auto write_text_locked(output_channel channel, std::string_view text)
    -> expected<std::size_t> {
  return write_all_locked(channel, {text.data(), text.size()});
}
#endif

/** @brief 格式化入口拥有整个记录，直到单次串行输出安全结束。 */
inline auto write_owned(output_channel channel,
                        std::string text,
                        io::operation_options options = {}) -> task<expected<void>> {
  auto result = co_await perform(
      shared_output(channel),
      [channel, text = std::move(text)] { return write_text_locked(channel, text); },
      options);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}

/** @brief 读取任务拥有输入状态，流门面可在等待前销毁。 */
inline auto read(std::shared_ptr<input_state> state,
                 std::span<char> bytes,
                 io::operation_options options) -> task<expected<std::size_t>> {
  co_return co_await perform(state, [state, bytes] { return read_locked(*state, bytes); }, options);
}

/** @brief 缓冲区由任务帧持有，读取安全完成后交回有效字节数。 */
inline auto read_owned(std::shared_ptr<input_state> state,
                       io::io_buffer buffer,
                       io::operation_options options) -> task<expected<io::io_transfer>> {
  auto result = co_await read(std::move(state), buffer.writable_bytes(), options);
  if (!result)
    co_return std::unexpected{result.error()};
  buffer.set_size(*result);
  co_return io::io_transfer{std::move(buffer), *result};
}

/** @brief 用共享预读缓冲读取一行；EOF 返回 nullopt，空行保留换行字节。 */
inline auto read_line(std::shared_ptr<input_state> state,
                      std::size_t max_bytes,
                      io::operation_options options) -> task<expected<std::optional<std::string>>> {
  if (!max_bytes)
    co_return std::unexpected{make_error(EINVAL)};
  std::optional<std::string> line;
  auto result = co_await perform(
      state,
      [state, &line, max_bytes] { return read_line_locked(*state, line, max_bytes); },
      options);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return std::move(line);
}

/** @brief 一个调用只写入一批，允许短写；任务不借用输出流门面。 */
inline auto write(output_channel channel,
                  std::span<const char> bytes,
                  io::operation_options options) -> task<expected<std::size_t>> {
  co_return co_await perform(
      shared_output(channel), [channel, bytes] { return native_write(channel, bytes); }, options);
}

/** @brief 保留拥有型缓冲区，单次写入完成后交回所有权。 */
inline auto write_buffer(output_channel channel,
                         io::io_buffer buffer,
                         io::operation_options options) -> task<expected<io::io_transfer>> {
  auto result = co_await write(channel, buffer.bytes(), options);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return io::io_transfer{std::move(buffer), *result};
}

/** @brief 在一次通道锁内循环写完借用字节区间。 */
inline auto write_all(output_channel channel,
                      std::span<const char> bytes,
                      io::operation_options options) -> task<expected<void>> {
  auto result = co_await perform(
      shared_output(channel),
      [channel, bytes] { return write_all_locked(channel, bytes); },
      options);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}

/** @brief 排空 C 标准流已有缓冲，不强制磁盘持久化。 */
inline auto flush(output_channel channel, io::operation_options options) -> task<expected<void>> {
  auto result = co_await perform(
      shared_output(channel),
      [channel]() -> expected<std::size_t> {
#if defined(_WIN32)
        const auto result = std::fflush(output_file(channel));
#else
        const auto result = output_call(channel, [&] { return std::fflush(output_file(channel)); });
#endif
        if (result != 0)
          return std::unexpected{make_error(errno ? errno : EIO)};
        return 0;
      },
      options);
  if (!result)
    co_return std::unexpected{result.error()};
  co_return expected<void>{};
}

/** @brief 输出门面的共用实现；所有任务只按值保存通道编号。 */
class output_stream {
 public:
  /** @brief 借用进程标准输出通道。 */
  explicit output_stream(output_channel channel) noexcept : channel_(channel) {}

  /** @brief 写一批借用字节；调用方保留缓冲到任务完成。 */
  auto write(std::span<const char> bytes, io::operation_options options = {}) const
      -> task<expected<std::size_t>> {
    return detail::write(channel_, bytes, options);
  }

  /** @brief 移交拥有型缓冲，完成后归还所有权和写入长度。 */
  auto write(io::io_buffer buffer, io::operation_options options = {}) const
      -> task<expected<io::io_transfer>> {
    return detail::write_buffer(channel_, std::move(buffer), options);
  }

  /** @brief 写完借用字节；整个调用与本通道其他写入串行。 */
  template <class Character, std::size_t Extent>
    requires std::same_as<std::remove_const_t<Character>, char>
  auto write_all(std::span<Character, Extent> bytes, io::operation_options options = {}) const
      -> task<expected<void>> {
    return detail::write_all(channel_, {bytes.data(), bytes.size()}, options);
  }

  /** @brief 调用时复制文本，使临时字符串和字符串视图可安全延迟等待。 */
  auto write_all(std::string_view text, io::operation_options options = {}) const
      -> task<expected<void>> {
    return write_owned(channel_, std::string{text}, options);
  }

  /** @brief 右值字符串直接转交给输出任务，左值复制后由任务持有。 */
  template <class Text>
    requires std::same_as<std::remove_cvref_t<Text>, std::string>
  auto write_all(Text&& text, io::operation_options options = {}) const -> task<expected<void>> {
    return write_owned(channel_, std::forward<Text>(text), options);
  }

  /** @brief 排空本通道的 C 标准流缓冲。 */
  auto flush(io::operation_options options = {}) const -> task<expected<void>> {
    return detail::flush(channel_, options);
  }

 private:
  output_channel channel_;
};
}  // namespace detail

/** @brief 标准输入的共享协程门面，不拥有或关闭进程输入句柄。 */
class stdin_stream {
 public:
  /** @brief 创建共享标准输入门面。 */
  stdin_stream() : state_(detail::shared_input()) {}

  /** @brief 返回首批可读字节，优先使用共享预读缓冲。 */
  auto read(std::span<char> bytes, io::operation_options options = {}) const
      -> task<expected<std::size_t>> {
    return detail::read(state_, bytes, options);
  }

  /** @brief 读取后归还拥有型缓冲区和有效字节数。 */
  auto read(io::io_buffer buffer, io::operation_options options = {}) const
      -> task<expected<io::io_transfer>> {
    return detail::read_owned(state_, std::move(buffer), options);
  }

  /** @brief 保留行尾换行；EOF 为 nullopt，超出长度限制返回 EMSGSIZE。 */
  auto read_line(std::size_t max_bytes = 1024 * 1024, io::operation_options options = {}) const
      -> task<expected<std::optional<std::string>>> {
    return detail::read_line(state_, max_bytes, options);
  }

 private:
  std::shared_ptr<detail::input_state> state_;
};

/** @brief 标准输出的协程门面，不接管标准句柄。 */
class stdout_stream : public detail::output_stream {
 public:
  stdout_stream() noexcept : output_stream(detail::output_channel::STANDARD_OUTPUT) {}
};

/** @brief 标准错误的协程门面，不接管标准句柄。 */
class stderr_stream : public detail::output_stream {
 public:
  stderr_stream() noexcept : output_stream(detail::output_channel::STANDARD_ERROR) {}
};

/** @brief 取得共享标准输入流。 */
inline auto in() -> stdin_stream {
  return {};
}

/** @brief 取得标准输出流。 */
inline auto out() noexcept -> stdout_stream {
  return {};
}

/** @brief 取得标准错误流。 */
inline auto err() noexcept -> stderr_stream {
  return {};
}

/** @brief 取得共享标准输入流。 */
inline auto standard_input() -> stdin_stream {
  return in();
}

/** @brief 取得标准输出流。 */
inline auto standard_output() noexcept -> stdout_stream {
  return out();
}

/** @brief 取得标准错误流。 */
inline auto standard_error() noexcept -> stderr_stream {
  return err();
}
}  // namespace faio::stdio
