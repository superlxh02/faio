/**
 * @file examples/quickstart.cpp
 * @brief 异步任务入门示例。
 */
#include <faio/faio.hpp>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

/**
 * @brief 异步等待一小段时间后返回给定整数。
 * @param value 等待后要交给调用方的值。
 * @return 到期时完成的任务。等待期间可以推进其他任务。
 */
faio::task<int> delayed_value(int value) {
  co_await faio::time::sleep(10ms);
  co_return value;
}

/**
 * @brief 提交第一个任务，再并发等待另外两个任务。
 * @return 三个任务返回值之和。
 * @details spawn 返回结果句柄，join 等两条分支结束后按参数顺序交付结果。
 */
faio::task<int> answer() {
  auto first = faio::spawn(delayed_value(20));
  const auto a = co_await first;

  auto [b, c] = co_await faio::join(delayed_value(11), delayed_value(11));
  co_return a + b + c;
}

/** @brief 在同步入口等待 answer 完成，然后打印 42。 */
int main() {
  const auto result = faio::block_on(answer());
  std::cout << "The answer is " << result << '\n';
  return result == 42 ? 0 : 1;
}
