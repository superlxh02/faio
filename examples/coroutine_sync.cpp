/**
 * 协程同步：每个示例一个入口函数，所有共享数据都属于当前示例。
 * 这些原语在等待时挂起协程，不像 std::mutex / std::condition_variable
 * 的等待那样占住调度线程。示例用 join/join_all 保证子任务结束后再销毁数据。
 */
#include "faio/faio.hpp"
#include "faio/log.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
// 1. mutex：分别用手动加解锁和 guard 完成相同的并发库存更新。
void example_mutex() {
  // 写法一：co_await lock() 异步获取锁，然后明确调用 unlock()。
  {
    faio::sync::mutex mutex;
    int inventory = 0;
    auto restock = [](faio::sync::mutex &lock, int &stock) -> faio::task<void> {
      for (int i = 0; i < 100; ++i) {
        co_await lock.lock(); // 争用时挂起协程，不阻塞 worker。
        ++stock;
        lock.unlock();
        // 这段临界区只有不会抛异常的整数递增。手动写法必须保证所有退出路径
        // 都释放锁；若临界区包含可能抛异常的业务操作，优先采用下方 guard 写法。
        co_await faio::this_coro::yield();
      }
    };
    faio::block_on(faio::join(restock(mutex, inventory),
                              restock(mutex, inventory),
                              restock(mutex, inventory)));
    if (inventory != 300)
      throw std::runtime_error("mutex 手动加解锁的库存数量错误");
    faio::log::logger()->info("1. mutex 手动 lock/unlock：库存 {}", inventory);
  }

  // 写法二：scoped_lock() 获取锁并返回 mutex::guard，作用域结束自动解锁。
  {
    faio::sync::mutex mutex;
    int inventory = 0;
    auto restock = [](faio::sync::mutex &lock, int &stock) -> faio::task<void> {
      for (int i = 0; i < 100; ++i) {
        {
          auto guard = co_await lock.scoped_lock();
          ++stock;
          // guard 析构时解锁，即使这里 return 或抛异常也会归还锁。
          // mutex::guard 本身不执行加锁；scoped_lock() 负责先获取锁再构造
          // guard。
        }
        co_await faio::this_coro::yield();
      }
    };
    faio::block_on(faio::join(restock(mutex, inventory),
                              restock(mutex, inventory),
                              restock(mutex, inventory)));
    if (inventory != 300)
      throw std::runtime_error("mutex guard 的库存数量错误");
    faio::log::logger()->info("   mutex scoped_lock/guard：库存 {}", inventory);
  }
}

// 2. condition_variable：两个请求等待配置加载完成。
void example_condition_variable() {
  faio::sync::mutex mutex;
  faio::sync::condition_variable ready_event;
  bool ready = false;
  std::string configuration;
  auto request = [](int id, faio::sync::mutex &lock,
                    faio::sync::condition_variable &event, bool &loaded,
                    std::string &config) -> faio::task<void> {
    std::string snapshot;
    {
      auto guard = co_await lock.scoped_lock();
      // wait 会释放锁并挂起，醒来后重新获取锁、检查谓词。
      // 必须检查状态而非只等通知：即使加载先完成，也不会漏掉通知而永久等待。
      co_await event.wait(lock, [&loaded] { return loaded; });
      snapshot = config;
    }
    faio::log::logger()->info("2. 请求 {} 使用配置：{}", id, snapshot);
  };
  auto load = [](faio::sync::mutex &lock, faio::sync::condition_variable &event,
                 bool &loaded, std::string &config) -> faio::task<void> {
    co_await faio::time::sleep(30ms);
    {
      auto guard = co_await lock.scoped_lock();
      config = "服务地址=127.0.0.1，重试次数=3";
      loaded = true;
    }
    event.notify_all(); // 状态先更新并解锁，再唤醒全部等待者。
  };
  faio::block_on(
      faio::join(request(1, mutex, ready_event, ready, configuration),
                 request(2, mutex, ready_event, ready, configuration),
                 load(mutex, ready_event, ready, configuration)));
}

// 3. semaphore：限制同时进行的异步下载数量，许可跨越 co_await 持有。
void example_semaphore() {
  faio::sync::semaphore slots{2};
  std::atomic<int> active{0};
  std::atomic<int> peak{0};
  auto download = [](int id, faio::sync::semaphore &limit,
                     std::atomic<int> &running,
                     std::atomic<int> &maximum) -> faio::task<void> {
    auto permit = co_await limit.acquire_permit();
    const int count = running.fetch_add(1) + 1;
    int observed = maximum.load();
    while (count > observed &&
           !maximum.compare_exchange_weak(observed, count)) {
    }
    faio::log::logger()->info("3. 下载 {} 开始，当前并发 {}", id, count);
    co_await faio::time::sleep(20ms);
    running.fetch_sub(1);
    // permit 析构自动归还许可；其他等待下载才可以继续。
  };
  std::vector<faio::task<void>> downloads;
  for (int id = 1; id <= 5; ++id)
    downloads.push_back(download(id, slots, active, peak));
  faio::block_on(faio::join_all(std::move(downloads)));
  if (peak.load() > 2 || active.load() != 0)
    throw std::runtime_error("semaphore 示例并发限制错误");
  faio::log::logger()->info("   全部下载结束，最大并发 {}", peak.load());
}

// 4. latch：三个初始化步骤结束后，一次性开放服务。
void example_latch() {
  faio::sync::latch initialized{3};
  auto initialize = [](int id, faio::sync::latch &done) -> faio::task<void> {
    co_await faio::time::sleep(id * 10ms);
    faio::log::logger()->info("4. 初始化步骤 {} 完成", id);
    done.count_down();
  };
  auto open_service = [](faio::sync::latch &done) -> faio::task<void> {
    co_await done
        .wait(); // 计数归零才恢复；归零后不能重置，后续 wait 立即完成。
    faio::log::logger()->info("   初始化全部完成，可以接受请求");
  };
  faio::block_on(
      faio::join(initialize(1, initialized), initialize(2, initialized),
                 initialize(3, initialized), open_service(initialized)));
}

// 5. barrier：三个分片分两轮处理，每轮全部结束才进入下一轮。
void example_barrier() {
  faio::sync::barrier phase_done{3};
  auto shard = [](int id, faio::sync::barrier &barrier) -> faio::task<void> {
    for (int round = 1; round <= 2; ++round) {
      co_await faio::time::sleep(id * 10ms);
      faio::log::logger()->info("5. 分片 {} 完成第 {} 轮", id, round);
      co_await barrier.arrive_and_wait();
      // barrier 与 latch 不同，可以反复使用；最后到达者释放当前轮所有等待者。
    }
  };
  faio::block_on(faio::join(shard(1, phase_done), shard(2, phase_done),
                            shard(3, phase_done)));
}

// 6. mpsc：两个生产者提交订单，一个消费者处理，容量 2 展示背压。
void example_mpsc() {
  auto [sender, receiver] = faio::sync::mpsc<int>::make(2);
  auto produce = [](faio::sync::mpsc<int>::sender output,
                    int first) -> faio::task<void> {
    for (int id = first; id < first + 3; ++id) {
      // 队列满时 send 挂起生产者，避免无限堆积数据。
      const auto sent = co_await output.send(id);
      if (!sent)
        throw std::runtime_error(std::string{sent.error().message()});
    }
    // sender 是可复制端点；最后一个 sender
    // 析构后，消费者排空队列再得到关闭错误。
  };
  auto consume = [](faio::sync::mpsc<int>::receiver input) -> faio::task<int> {
    int total = 0;
    for (;;) {
      auto order = co_await input.recv();
      if (!order) {
        if (order.error().value() == faio::Error::ClosedChannel)
          co_return total;
        throw std::runtime_error(std::string{order.error().message()});
      }
      faio::log::logger()->info("6. 消费者处理订单 {}", *order);
      ++total;
      co_await faio::time::sleep(10ms); // 消费慢于生产，使小容量队列产生背压。
    }
  };
  // 先复制一份，再把两份都移动给生产者。入口不能留下多余 sender，否则 recv
  // 无法在生产者结束后识别关闭。receiver 只允许一个，移动给唯一消费者。
  auto second_sender = sender;
  auto [first_done, second_done, total] = faio::block_on(faio::join(
      produce(std::move(sender), 100), produce(std::move(second_sender), 200),
      consume(std::move(receiver))));
  (void)first_done;
  (void)second_done;
  if (total != 6)
    throw std::runtime_error("mpsc 示例订单数量错误");
  faio::log::logger()->info("   两个生产者退出、队列排空，共处理 {} 个订单",
                            total);
}
} // namespace

int main(int argc, char **argv) {
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
    } else if (const char *environment = std::getenv("FAIO_TEST_IO_BACKEND")) {
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
      throw std::invalid_argument(
          "本平台使用固定 IO 后端，无需选择 Linux 后端");
#endif
    faio::runtime::configure(builder.set_num_workers(2).build());
    example_mutex();
    example_condition_variable();
    example_semaphore();
    example_latch();
    example_barrier();
    example_mpsc();
    faio::runtime::shutdown();
  } catch (const std::exception &error) {
    faio::log::logger()->error("同步示例失败：{}", error.what());
    return 1;
  }
}
