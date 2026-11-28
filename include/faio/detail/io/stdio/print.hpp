/**
 * @file faio/detail/io/stdio/print.hpp
 * @brief 格式化标准输出、错误输出和任意 faio 异步写入流。
 */
#pragma once

#include "faio/detail/io/stdio/format.hpp"
#include "faio/detail/io/stdio/stream.hpp"
#include "faio/detail/io/stream/algorithms.hpp"
#include <string>
#include <utility>

namespace faio::stdio {
namespace detail {
/** @brief 格式化数据由协程帧持有；目标流由调用方保持到操作完成。 */
template <io::borrowed_async_writer Writer>
task<expected<void>> write_formatted(Writer& writer, std::string text) {
  if constexpr (requires {
                  { writer.write_all(std::move(text)) } -> io::io_awaitable_of<void>;
                })
    co_return co_await writer.write_all(std::move(text));
  else
    co_return co_await io::write_all(writer, std::string_view{text});
}
}  // namespace detail

/**
 * @brief 输出一次完整格式化记录；格式化在调用时完成，I/O 在等待时执行。
 * @details 参数不跨挂起点借用。格式化异常同步传播，I/O 错误通过 expected 返回。
 */
template <class... Args>
[[nodiscard]] task<expected<void>> print(format_string<Args...> text, Args&&... args) {
  return detail::write_owned(detail::output_channel::STANDARD_OUTPUT,
                             faio::stdio::format(text, std::forward<Args>(args)...));
}

/** @brief 使用附加停止令牌和安全排空的软截止时间输出一条记录。 */
template <class... Args>
[[nodiscard]] task<expected<void>> print(io::operation_options options,
                                         format_string<Args...> text,
                                         Args&&... args) {
  return detail::write_owned(detail::output_channel::STANDARD_OUTPUT,
                             faio::stdio::format(text, std::forward<Args>(args)...),
                             std::move(options));
}

/** @brief 输出格式化记录并追加换行，整条记录共享同一次通道写入。 */
template <class... Args>
[[nodiscard]] task<expected<void>> println(format_string<Args...> text, Args&&... args) {
  auto output = faio::stdio::format(text, std::forward<Args>(args)...);
  output += '\n';
  return detail::write_owned(detail::output_channel::STANDARD_OUTPUT, std::move(output));
}

/** @brief 带取消和软截止时间输出格式化记录及换行。 */
template <class... Args>
[[nodiscard]] task<expected<void>> println(io::operation_options options,
                                           format_string<Args...> text,
                                           Args&&... args) {
  auto output = faio::stdio::format(text, std::forward<Args>(args)...);
  output += '\n';
  return detail::write_owned(
      detail::output_channel::STANDARD_OUTPUT, std::move(output), std::move(options));
}

/** @brief 仅向标准输出写入换行。 */
[[nodiscard]] inline task<expected<void>> println(io::operation_options options = {}) {
  return detail::write_owned(detail::output_channel::STANDARD_OUTPUT, "\n", std::move(options));
}

/** @brief 向标准错误输出一次完整格式化记录。 */
template <class... Args>
[[nodiscard]] task<expected<void>> eprint(format_string<Args...> text, Args&&... args) {
  return detail::write_owned(detail::output_channel::STANDARD_ERROR,
                             faio::stdio::format(text, std::forward<Args>(args)...));
}

/** @brief 带取消和软截止时间向标准错误输出记录。 */
template <class... Args>
[[nodiscard]] task<expected<void>> eprint(io::operation_options options,
                                          format_string<Args...> text,
                                          Args&&... args) {
  return detail::write_owned(detail::output_channel::STANDARD_ERROR,
                             faio::stdio::format(text, std::forward<Args>(args)...),
                             std::move(options));
}

/** @brief 向标准错误输出格式化记录并追加换行。 */
template <class... Args>
[[nodiscard]] task<expected<void>> eprintln(format_string<Args...> text, Args&&... args) {
  auto output = faio::stdio::format(text, std::forward<Args>(args)...);
  output += '\n';
  return detail::write_owned(detail::output_channel::STANDARD_ERROR, std::move(output));
}

/** @brief 带取消和软截止时间向标准错误输出记录及换行。 */
template <class... Args>
[[nodiscard]] task<expected<void>> eprintln(io::operation_options options,
                                            format_string<Args...> text,
                                            Args&&... args) {
  auto output = faio::stdio::format(text, std::forward<Args>(args)...);
  output += '\n';
  return detail::write_owned(
      detail::output_channel::STANDARD_ERROR, std::move(output), std::move(options));
}

/** @brief 仅向标准错误写入换行。 */
[[nodiscard]] inline task<expected<void>> eprintln(io::operation_options options = {}) {
  return detail::write_owned(detail::output_channel::STANDARD_ERROR, "\n", std::move(options));
}

/** @brief 格式化到借用的异步写入流；调用方保持 writer 存活到操作完成。 */
template <io::borrowed_async_writer Writer, class... Args>
[[nodiscard]] task<expected<void>> print_to(Writer& writer,
                                            format_string<Args...> text,
                                            Args&&... args) {
  return detail::write_formatted(writer, faio::stdio::format(text, std::forward<Args>(args)...));
}

/** @brief 格式化到借用的异步写入流并追加换行。 */
template <io::borrowed_async_writer Writer, class... Args>
[[nodiscard]] task<expected<void>> println_to(Writer& writer,
                                              format_string<Args...> text,
                                              Args&&... args) {
  auto output = faio::stdio::format(text, std::forward<Args>(args)...);
  output += '\n';
  return detail::write_formatted(writer, std::move(output));
}

/** @brief 向借用的异步写入流写入换行。 */
template <io::borrowed_async_writer Writer>
[[nodiscard]] task<expected<void>> println_to(Writer& writer) {
  return detail::write_formatted(writer, "\n");
}
}  // namespace faio::stdio
