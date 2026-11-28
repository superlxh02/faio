/**
 * @file stdio/console.cpp
 * @brief 格式化输出结果与可选值，并显式刷新标准输出。
 */
#include <faio/faio.hpp>

#include <exception>
#include <iostream>
#include <optional>

namespace {
/** @brief 输出完整记录；每一步检查标准流返回的 I/O 错误。 */
faio::task<int> demonstrate() {
  const faio::expected<int> answer{42};
  const std::optional<int> selected{7};
  const auto prefix = co_await faio::stdio::print("answer={} ", answer);
  if (!prefix)
    co_return 1;
  const auto line = co_await faio::stdio::println("selected={}", selected);
  if (!line)
    co_return 1;

  const auto diagnostic = co_await faio::stdio::eprintln("stdio example completed");
  if (!diagnostic)
    co_return 1;

  auto output = faio::stdio::out();
  co_return (co_await output.flush()) ? 0 : 1;
}
}  // namespace

/** @brief 运行无需交互输入的标准流示例。 */
int main() {
  try {
    return faio::block_on(demonstrate());
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
