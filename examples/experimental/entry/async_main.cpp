/**
 * @file examples/experimental/entry/async_main.cpp
 * @brief 用注解声明异步入口，读取命令行参数并交付进程退出状态。
 */
#include <coroutine>
#include <iostream>
#include <string_view>

#if defined(FAIO_USE_MODULES) && FAIO_USE_MODULES
#include <meta>
import faio.experimental;
#include <faio-module/entry.hpp>
#else
#include <faio/faio.hpp>
#include <faio/experimental/async_main.h>
#endif

namespace {
/**
 * @brief 让出一次执行权，再交付计算结果。
 * @param value 该项任务要交付的整数。
 * @return 恢复执行后得到的值。
 */
faio::task<int> calculate(int value) {
  co_await faio::this_coro::yield();
  co_return value;
}
}  // namespace

/**
 * @brief 接收一个可选的称呼，并发计算两部分结果。
 * @param argc 参数个数，包含程序名。省略称呼时使用默认值。
 * @param argv 参数列表。程序只接受一个非空称呼。
 * @return 成功返回 0，参数错误返回 2，计算结果有误返回 1。
 * @details 注解生成程序入口，将命令行参数交给此协程，并把 co_return 的整数用作退出状态。
 * 称呼借用命令行存储，使用范围限定在入口任务内。
 */
[[= faio::experimental::main(app)]] faio::task<int> app(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && std::string_view{argv[1]}.empty())) {
    std::cerr << "用法：" << argv[0] << " [称呼]\n";
    co_return 2;
  }

  const std::string_view name = argc == 2 ? argv[1] : "faio";
  const auto [left, right] = co_await faio::join(calculate(20), calculate(22));
  const int result = left + right;
  std::cout << name << "，计算结果为 " << result << '\n';
  co_return result == 42 ? 0 : 1;
}
