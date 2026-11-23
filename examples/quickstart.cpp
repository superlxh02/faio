/**
 * @file examples/quickstart.cpp
 * @brief 第一个异步程序：组合一个任务，等待它输出问候语。
 */
#include <faio/faio.hpp>

#include <chrono>
#include <print>

using namespace std::chrono_literals;

/** @brief 挂起 10 毫秒，然后输出 `Hello, world!`。 */
faio::task<void> hello() {
  co_await faio::time::sleep(10ms);
  std::print("Hello, world!\n");
}

/** @brief 组合 hello 任务并等待它完成。 */
faio::task<void> run() {
  co_await faio::join(hello());
}

/** @brief 在同步入口驱动任务直到完成。 */
int main() {
  faio::block_on(run());
}
