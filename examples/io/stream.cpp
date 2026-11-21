/**
 * @file examples/io/stream.cpp
 * @brief 在内存流中演示缓冲写入、完整读取和 EOF 的区别。
 */
#include <faio/faio.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

/**
 * @brief 检查一次 I/O 操作，在失败时报告调用环节和错误消息。
 * @tparam T 成功结果的类型，可以是 void。
 * @param result 等待 I/O 操作所得的结果。
 * @param step 出错时打印的调用环节。
 * @return 成功结果的值。
 */
template <class T>
T checked(faio::expected<T> result, std::string_view step) {
  if (!result)
    throw std::runtime_error(std::string{step} + "失败：" + std::string{result.error().message()});
  if constexpr (!std::is_void_v<T>)
    return std::move(*result);
}

/** @brief 写入一段消息，再从流的开头完整读回并检查结尾。 */
faio::task<void> demonstrate() {
  constexpr std::string_view message{"hello faio"};
  faio::io::MemoryStream stream;
  {
    faio::io::BufWriter writer{stream, 32};
    /** @brief 完整写入算法会处理短写。字节写入缓冲区后，仍需显式等待 flush 将其提交。 */
    checked(co_await faio::io::write_all(writer, message), "写入");
    checked(co_await writer.flush(), "刷新缓冲");
  }

  /** @brief MemoryStream 的读写共用游标。重置位置后再创建借用它的读取适配器。 */
  checked(co_await stream.seek(0), "定位到开头");
  faio::io::BufReader reader{stream, 4};
  std::array<char, message.size()> received{};
  checked(co_await faio::io::read_exact(reader, received), "读回完整消息");
  const std::string_view text{received.data(), received.size()};
  if (text != message)
    throw std::runtime_error("读回的字节与写入内容不一致");

  /** @brief 缓冲区非空且读取成功但返回 0，表示 EOF。read_exact 遇到提前 EOF 则会报告错误。 */
  std::array<char, 1> tail{};
  if (checked(co_await reader.read(tail), "检查流结尾") != 0)
    throw std::runtime_error("完整消息后还有额外字节");
  std::cout << text << '\n';
}

}  // namespace

/** @brief 等待内存流示例，遇到错误时输出原因。 */
int main() {
  try {
    faio::block_on(demonstrate());
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "内存流示例失败：" << error.what() << '\n';
    return 1;
  }
}
