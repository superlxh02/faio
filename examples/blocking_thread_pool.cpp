/**
 * 阻塞线程池场景：商品详情同时查询旧仓库 SDK（同步阻塞）和价格服务（异步）。
 * 只配置一个协程 worker，让日志直接展示：SDK 阻塞时，异步查询与心跳仍然推进。
 */
#include "faio/faio.hpp"
#include "faio/log.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace {
// 这是普通同步函数，不返回 task。模拟没有异步接口的旧 SDK。
int legacy_inventory_query(int product_id) {
  faio::log::logger()->info("阻塞线程池：查询商品 {} 的库存，SDK 将阻塞 400ms", product_id);
  std::this_thread::sleep_for(400ms);  // 阻塞只发生在 spawn_blocking 的线程池内。
  return 12;
}

faio::task<int> async_price_query(int product_id, const std::atomic<bool>& inventory_done) {
  faio::log::logger()->info("协程 worker：开始异步查询商品 {} 的价格", product_id);
  // 实际应用中这里可以 co_await TcpStream 的读写。为了离线可运行，用异步
  // 定时器模拟网络等待；它不阻塞线程，也没有理由转交给 spawn_blocking。
  co_await faio::time::sleep(80ms);
  faio::log::logger()->info("协程 worker：价格查询完成，库存调用已完成 = {}",
                            inventory_done.load());
  co_return 299;
}

faio::task<void> heartbeat(std::atomic<bool>& inventory_done) {
  for (int tick = 1; tick <= 6; ++tick) {
    co_await faio::time::sleep(50ms);
    faio::log::logger()->info("协程心跳 {}：库存调用已完成 = {}", tick, inventory_done.load());
  }
}

void example_blocking_and_async() {
  // 状态放在 block_on 外，覆盖它所派生任务的生命周期，异常路径也不会悬空。
  std::atomic<bool> inventory_done{false};
  faio::block_on([](std::atomic<bool>& done) -> faio::task<void> {
    // 三个任务独立启动：只有旧 SDK 的普通阻塞函数交给阻塞线程池。
    // spawn_blocking 按值保存这个普通 lambda；与临时协程 lambda
    // 的生命周期不同。
    auto inventory = faio::spawn_blocking([&done] {
      const int count = legacy_inventory_query(1001);
      done.store(true);
      return count;
    });
    auto price = faio::spawn(async_price_query(1001, done));
    auto ticker = faio::spawn(heartbeat(done));
    // 等库存时挂起当前协程，不占住唯一 worker，price 和 ticker 可以继续执行。
    // 这里不能 inventory.get()；get 是普通线程使用的同步阻塞接口。
    const int stock = co_await inventory;
    const int unit_price = co_await price;
    co_await ticker;
    faio::log::logger()->info("商品详情汇合：库存 {}，单价 {}", stock, unit_price);
  }(inventory_done));
}
}  // namespace

int main(int argc, char** argv) {
  try {
    // 后端参数在本文件内处理，示例不依赖其他 example 的工具函数。
    auto builder = faio::config_builder{};
#if defined(__linux__)
    if (argc > 2)
      throw std::invalid_argument("仅支持 --io-backend=epoll|uring");
    std::string_view selection;
    if (argc == 2) {
      constexpr std::string_view prefix{"--io-backend="};
      const std::string_view argument{argv[1]};
      if (!argument.starts_with(prefix) || argument.size() == prefix.size())
        throw std::invalid_argument("请使用 --io-backend=epoll|uring");
      selection = argument.substr(prefix.size());
    } else if (const char* environment = std::getenv("FAIO_TEST_IO_BACKEND")) {
      selection = environment;
    }
    if (selection == "epoll")
      builder.set_io_backend(faio::runtime::io_backend::IO_EPOLL);
    else if (selection == "uring")
      builder.set_io_backend(faio::runtime::io_backend::IO_URING);
    else if (!selection.empty())
      throw std::invalid_argument("IO 后端必须是 epoll 或 uring");
#else
    (void)argv;
    if (argc > 1)
      throw std::invalid_argument("本平台使用固定 IO 后端，无需选择 Linux 后端");
#endif
    faio::runtime::configure(builder
                                 .set_num_workers(1)           // 唯一协程 worker 不执行阻塞 SDK。
                                 .set_max_blocking_threads(2)  // 独立阻塞池的线程上限。
                                 .set_blocking_keep_alive(5s)
                                 .build());
    example_blocking_and_async();
    faio::runtime::shutdown();
  } catch (const std::exception& error) {
    faio::log::logger()->error("阻塞线程池示例失败：{}", error.what());
    return 1;
  }
}
