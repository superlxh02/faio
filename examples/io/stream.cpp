/**
 * @file examples/io/stream.cpp
 * @brief 在内存流中演示缓冲写入、完整读取和 EOF 的区别。
 */
#include <faio/faio.hpp>

#include <array>
#include <exception>
#include <string_view>

namespace {
/** @brief 写入一段消息，再从流的开头完整读回并检查结尾。 */
faio::task<int> demonstrate() {
  constexpr std::string_view message{"hello faio"};
  faio::io::memory_stream stream;
  {
    faio::io::buf_writer writer{stream, 32};
    const auto written = co_await faio::io::write_all(writer, message);
    if (!written) {
      faio::log::logger()->error("写入失败：{}", written.error().message());
      co_return 1;
    }
    const auto flushed = co_await writer.flush();
    if (!flushed) {
      faio::log::logger()->error("刷新缓冲失败：{}", flushed.error().message());
      co_return 1;
    }
  }

  const auto seeked = co_await stream.seek(0);
  if (!seeked) {
    faio::log::logger()->error("定位失败：{}", seeked.error().message());
    co_return 1;
  }

  faio::io::buf_reader reader{stream, 4};
  std::array<char, message.size()> received{};
  const auto got = co_await faio::io::read_exact(reader, received);
  if (!got) {
    faio::log::logger()->error("读回失败：{}", got.error().message());
    co_return 1;
  }

  const std::string_view text{received.data(), received.size()};
  if (text != message) {
    faio::log::logger()->error("读回的字节与写入内容不一致");
    co_return 1;
  }

  std::array<char, 1> tail{};
  const auto extra = co_await reader.read(tail);
  if (!extra) {
    faio::log::logger()->error("检查结尾失败：{}", extra.error().message());
    co_return 1;
  }
  if (*extra != 0) {
    faio::log::logger()->error("完整消息后还有额外字节");
    co_return 1;
  }

  faio::log::logger()->info("{}", text);
  co_return 0;
}
}  // namespace

/** @brief 等待内存流示例，遇到错误时输出原因。 */
int main() {
  try {
    return faio::block_on(demonstrate());
  } catch (const std::exception& error) {
    faio::log::logger()->error("内存流示例失败：{}", error.what());
    return 1;
  }
}
