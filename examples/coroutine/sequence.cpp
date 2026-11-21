/**
 * @file sequence.cpp
 * @brief 逐项读到 EOF、提前停止读取，以及限制异步序列消费的并发数。
 */
#include <faio/faio.hpp>

#include <atomic>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
/**
 * @brief 将一批编号逐项交给流。
 * @param output 本次流操作借出的生产端，作用域由流组合管理。
 */
faio::task<void> produce(faio::stream_producer<int>& output) {
  for (int value = 1; value <= 5; ++value) {
    /** @brief emit 可以挂起，让消费端有机会取走已提交的项目。 */
    co_await output.emit(value);
  }
}

/**
 * @brief 只读第一项，验证 with_stream 在返回前已等生产者停止。
 * @details emit 交付第一项后等待下一次拉取。消费者无需发起第二次读取，
 * with_stream 在其退出时请求生产者停止，并等待生产者退出。
 */
faio::task<void> read_first_item() {
  std::atomic<bool> producer_stopped{false};
  const auto first = co_await faio::with_stream<int>(
      [&producer_stopped](faio::stream_producer<int>& output) -> faio::task<void> {
        try {
          co_await produce(output);
        } catch (const faio::operation_cancelled&) {
          producer_stopped.store(true);
          throw;
        }
      },
      [](faio::stream_reader<int>& input) -> faio::task<std::optional<int>> {
        co_return co_await input.next();
      });
  if (first != 1 || !producer_stopped.load())
    throw std::logic_error("提前退出读取后应停止生产者并等待它退出");
  std::cout << "只读取第一项 " << *first << "，生产者已停止并退出\n";
}

/** @brief 完整读取序列，再演示提前退出和限制并发数的消费方式。 */
faio::task<void> show_sequence() {
  co_await faio::with_stream<int>(produce, [](faio::stream_reader<int>& input) -> faio::task<void> {
    /** @brief next 在生产端正常结束后返回空值，循环此时自然结束。 */
    while (const auto value = co_await input.next()) {
      std::cout << "依次读取 " << *value << '\n';
    }
  });

  co_await read_first_item();

  std::atomic<int> sum{0};
  co_await faio::for_each_concurrent<int>(2, produce, [&sum](int value) -> faio::task<void> {
    co_await faio::this_coro::yield();
    /** @brief 多个处理任务可以同时修改求和结果，原子加法保证更新不丢失。 */
    sum.fetch_add(value, std::memory_order_relaxed);
  });
  const int total = sum.load(std::memory_order_relaxed);
  if (total != 15) {
    throw std::logic_error("异步序列未完整消费");
  }
  std::cout << "并发消费的总和为 " << total << '\n';
}
}  // namespace

/** @brief 顺序运行三种流消费方式，并报告错误。 */
int main() {
  try {
    faio::block_on(show_sequence());
  } catch (const std::exception& error) {
    std::cerr << "异步序列示例失败：" << error.what() << '\n';
    return 1;
  }
}
