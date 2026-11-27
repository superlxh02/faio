/**
 * @file examples/module/quickstart.cpp
 * @brief faio 模块支持入门示例，只需导入 faio。
 */
import faio;

faio::task<int> value(int result) {
  co_return result;
}

faio::task<int> answer() {
  const auto [left, right] = co_await faio::join(value(20), value(22));
  co_return left + right;
}

int main() {
  return faio::block_on(answer()) == 42 ? 0 : 1;
}
