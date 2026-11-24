/**
 * @file examples/module/quickstart.cpp
 * @brief 只导入 faio 的具名模块入门示例。
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
