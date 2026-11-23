/**
 * @file examples/runtime/execution.cpp
 * @brief 在等待异步价格的同时调用只有同步接口的库存 SDK。
 */
#include <faio/faio.hpp>

#include <chrono>
#include <thread>

namespace {
using namespace std::chrono_literals;

/**
 * @brief 模拟只有同步接口的库存查询。
 * @return 库存件数。
 * @details sleep_for 会阻塞调用线程。实际应用可在这里调用同步 SDK 或执行阻塞式计算。
 */
int query_stock() {
  std::this_thread::sleep_for(50ms);
  return 6;
}

/**
 * @brief 模拟本身已经提供异步接口的价格查询。
 * @return 等待完成后得到单价。
 */
faio::task<int> query_price() {
  co_await faio::time::sleep(10ms);
  co_return 7;
}

/**
 * @brief 启动两项查询并汇合结果。
 * @return 库存件数与单价的乘积。
 * @details spawn_blocking 把库存查询交给阻塞池，返回可等待的句柄。
 * 价格任务可以在库存查询期间推进，乘法只在两个结果都取得后执行。
 */
faio::task<int> inventory_value() {
  auto stock = faio::spawn_blocking(query_stock);
  const int price = co_await query_price();
  const int units = co_await stock;
  co_return units* price;
}
}  // namespace

/** @brief 等待库存估值并用退出状态校验结果。 */
int main() {
  const int value = faio::block_on(inventory_value());
  faio::log::logger()->info("库存总价值：{}", value);
  return value == 42 ? 0 : 1;
}
