/**
 * @file examples/module/quickstart.cpp
 * @brief 具名模块入门示例：导入 C++23 标准库模块与 faio 模块。
 * @details 示例默认使用 `import std;`，因此这个示例需要 GCC 16.1 及以上：
 * 构建系统只为该目标开启标准库模块，其他示例与其他工具链不受影响。
 */
import std;
import faio;

faio::task<void> hello() {
  co_await faio::time::sleep(std::chrono::milliseconds{10});
  std::print("Hello, world!\n");
}

faio::task<void> run() {
  co_await faio::join(hello());
}

int main() {
  faio::block_on(run());
}
