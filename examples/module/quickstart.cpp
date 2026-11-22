#include <chrono>
#include <coroutine>
#include <iostream>

import faio;

namespace {

faio::task<int> answer() {
  using namespace std::chrono_literals;
  co_await faio::time::sleep(10ms);
  co_return 42;
}

}  // namespace

int main() {
  const int value = faio::block_on(answer());
  std::cout << "The answer is " << value << '\n';
  return value == 42 ? 0 : 1;
}
