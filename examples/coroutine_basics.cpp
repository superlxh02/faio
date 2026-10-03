/**
 * 协程入门：每个 example_* 函数对应一个独立示例。
 * 阅读顺序：直接等待子协程 → 启动并发任务 → 后台任务 → 汇合 → 竞速。
 * task 是惰性的：调用协程函数只创建任务，co_await/block_on/spawn 才驱动它。
 */
#include "faio/faio.hpp"
#include "faio/log.hpp"
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace {
// 示例 1：一个协程直接 co_await 另一个协程，再由普通线程 block_on 取结果。
void example_await_and_block_on() {
  auto calculate_total = [](int quantity) -> faio::task<int> {
    auto query_price = [](int price, std::chrono::milliseconds delay) -> faio::task<int> {
      // 异步定时器挂起协程，不占住调度线程。
      co_await faio::time::sleep(delay);
      co_return price;
    };
    // 子协程返回之后才继续计算；直接 co_await 不会创建独立的并发任务。
    const int unit_price = co_await query_price(21, 20ms);
    co_return unit_price* quantity;
  };
  // block_on 是普通同步代码进入异步世界的入口，当前调用线程等待任务组结束。
  // 在协程里等待其他任务应写 co_await，不能再调用 block_on 或句柄 get()。
  const int total = faio::block_on(calculate_total(2));
  faio::log::logger()->info("1. co_await 子协程 + block_on：总价 = {}", total);
}

// 示例 2：spawn 立即提交两个独立任务，随后 co_await join_handle 获取结果。
faio::task<void> example_spawn_and_join_handle() {
  auto query_price = [](int price, std::chrono::milliseconds delay) -> faio::task<int> {
    // 异步定时器挂起协程，不占住调度线程。
    co_await faio::time::sleep(delay);
    co_return price;
  };
  auto first = faio::spawn(query_price(30, 80ms));
  auto second = faio::spawn(query_price(20, 30ms));
  faio::log::logger()->info("2. 两个价格查询已启动，当前协程可以继续处理其他工作");
  // 两个查询已经并发执行；先等 first 并不会让 second 延后启动。
  // co_await 句柄只挂起当前协程，worker 仍可执行其他协程。结果只能领取一次。
  const int a = co_await first;
  const int b = co_await second;
  faio::log::logger()->info("   co_await join_handle：价格分别为 {}、{}", a, b);
}

// 示例 3：无需领取返回值的后台通知，用 spawn_detached（API 名带 ed）。
void example_spawn_detached() {
  // 完成信号放在 block_on 外部，保证它覆盖后台任务的整个生命周期。
  faio::sync::latch notifications_done{2};
  faio::block_on([](faio::sync::latch& done) -> faio::task<void> {
    for (int order = 1; order <= 2; ++order) {
      // 无捕获协程 lambda 将参数存进协程帧，避免临时捕获闭包析构后悬空。
      faio::spawn_detached([](int id, faio::sync::latch& signal) -> faio::task<void> {
        // detached 不返回句柄，任务内部必须处理异常，否则未捕获异常会终止进程。
        try {
          co_await faio::time::sleep(20ms);
          faio::log::logger()->info("3. 后台通知：订单 {} 已发送确认消息", id);
        } catch (const std::exception& error) {
          faio::log::logger()->error("后台通知失败：{}", error.what());
        }
        signal.count_down();
      }(order, done));
    }
    faio::log::logger()->info("   主流程继续执行；没有通知任务的 join_handle");
    // 本演示为了看到完整输出而等待信号；spawn_detached 本身不要求立即等待。
    // block_on 也会排空本任务组内派生的后台任务。
    co_await done.wait();
  }(notifications_done));
}

// 示例 4：固定数量、可以有不同返回类型的任务，用 join 一次汇合。
faio::task<void> example_join() {
  auto query_price = [](int price, std::chrono::milliseconds delay) -> faio::task<int> {
    // 异步定时器挂起协程，不占住调度线程。
    co_await faio::time::sleep(delay);
    co_return price;
  };
  auto query_name = []() -> faio::task<std::string> {
    co_await faio::time::sleep(10ms);
    co_return "机械键盘";
  };
  // join 返回惰性组合任务，co_await 时才并发启动分支，不必预先 spawn。
  // 返回 tuple 保持参数顺序，与哪个分支先完成无关。void 分支对应 monostate。
  auto [name, price] = co_await faio::join(query_name(), query_price(299, 30ms));
  faio::log::logger()->info("4. join：商品 {}，价格 {}", name, price);
}

// 示例 5：运行时才知道查询数量，且各任务返回类型相同，用 join_all。
faio::task<void> example_join_all() {
  auto query_price = [](int price, std::chrono::milliseconds delay) -> faio::task<int> {
    // 异步定时器挂起协程，不占住调度线程。
    co_await faio::time::sleep(delay);
    co_return price;
  };
  std::vector<faio::task<int>> queries;
  for (int supplier = 0; supplier < 3; ++supplier)
    queries.push_back(query_price(100 + supplier * 10, (3 - supplier) * 10ms));
  // vector 内还只是惰性任务；join_all 等待全部任务并按输入顺序收集结果。
  const auto prices = co_await faio::join_all(std::move(queries));
  faio::log::logger()->info(
      "5. join_all：三个供应商报价 {}、{}、{}", prices[0], prices[1], prices[2]);
}

// 示例 6：查询和超时竞速，用 select 取得先完成的分支。
faio::task<void> example_select() {
  auto query_price = [](int price, std::chrono::milliseconds delay) -> faio::task<int> {
    // 异步定时器挂起协程，不占住调度线程。
    co_await faio::time::sleep(delay);
    co_return price;
  };
  auto deadline = []() -> faio::task<void> {
    co_await faio::time::sleep(100ms);
  };
  // 与 join 不同，select 只取先完成者，并请求其他分支取消、等待它们退出。
  // 本例 sleep 能响应取消；不会留下还在使用局部数据的慢分支。
  const auto winner = co_await faio::select(query_price(88, 20ms), deadline());
  // index 是原始参数下标；value 是 variant，即使类型重复也应使用下标访问。
  if (winner.index == 0)
    faio::log::logger()->info("6. select：查询先完成，报价 {}", std::get<0>(winner.value));
  else
    faio::log::logger()->info("6. select：等待报价超时");
  // 修改 query_price 的延时为 200ms，可以观察超时分支；被取消的 sleep
  // 会抛 operation_cancelled，由 select 收束失败分支后再返回。
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
    faio::runtime::configure(builder.set_num_workers(2).build());
    example_await_and_block_on();
    faio::block_on(example_spawn_and_join_handle());
    example_spawn_detached();
    faio::block_on(example_join());
    faio::block_on(example_join_all());
    faio::block_on(example_select());
    faio::runtime::shutdown();
  } catch (const std::exception& error) {
    faio::log::logger()->error("协程示例失败：{}", error.what());
    return 1;
  }
}
