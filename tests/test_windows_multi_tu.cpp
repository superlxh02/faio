/** @file test_windows_multi_tu.cpp
 * @brief 真实多 TU faio 消费、各 TU 独有动态 TLS 与跨 TU extern TLS 初始化。
 * @details 不使用 unity 构建；每个线程验证共享停止令牌和两个独立动态对象。
 */
#include "faio/faio.hpp"
#include "windows_multi_tu_support.hpp"
#include <atomic>
#include <iostream>
#include <stop_token>
#include <thread>
#include <vector>

thread_local std::string windows_tls_first = windows_tls_value("first");

int main() {
  std::atomic<int> failures{};
  std::vector<std::jthread> threads;
  for (int index = 0; index < 8; ++index) {
    threads.emplace_back([&] {
      // 第一次访问另一个 TU 的动态 TLS，必须调用那个 TU 原来的 initializer。
      if (windows_tls_first != "first" || windows_tls_second != "second")
        ++failures;
      std::stop_source source;
      faio::detail::current_stop_token = source.get_token();
      if (windows_second_observes_stop())
        ++failures;
      source.request_stop();
      if (!windows_second_observes_stop())
        ++failures;
      faio::detail::current_stop_token = {};
      if (windows_second_observes_stop())
        ++failures;
    });
  }
  threads.clear();
  if (windows_second_task_value() != 42 || failures.load())
    return 1;
  std::cout << "Windows multi-TU faio and dynamic TLS contracts passed\n";
}
