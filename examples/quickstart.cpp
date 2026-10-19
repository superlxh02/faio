#include <faio/faio.hpp>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

faio::task<int> delayed_value(int value) {
  co_await faio::time::sleep(10ms);
  co_return value;
}

faio::task<int> answer() {
  // spawn 立即提交任务，返回可等待的结果句柄
  auto first = faio::spawn(delayed_value(20));
  const auto a = co_await first;

  // join 把多个惰性任务组合成一次并发，按参数顺序返回结果
  auto [b, c] = co_await faio::join(delayed_value(11), delayed_value(11));
  co_return a + b + c;
}

int main() {
  const auto result = faio::block_on(answer());
  std::cout << "The answer is " << result << '\n';
  faio::runtime::shutdown();
}
