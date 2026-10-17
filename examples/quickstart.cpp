#include <faio/faio.hpp>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

faio::task<int> delayed_value(int value) {
  co_await faio::time::sleep(10ms);
  co_return value;
}

faio::task<int> answer() {
  auto [first, second] = co_await faio::join(
      delayed_value(20), delayed_value(22));
  co_return first + second;
}

int main() {
  const auto result = faio::block_on(answer());
  std::cout << "The answer is " << result << '\n';
  faio::runtime::shutdown();
}
