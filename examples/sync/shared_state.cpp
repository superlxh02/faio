/**
 * @file examples/sync/shared_state.cpp
 * @brief 同步上篇：用互斥锁发布共享配置，用信号量限制异步请求数。
 */
#include <faio/faio.hpp>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

namespace {

/** @brief 发布者和请求任务共同访问的配置。 */
struct SharedConfig {
  faio::sync::mutex mutex;                /**< 保护 value 的读写。 */
  faio::sync::condition_variable changed; /**< 配置就绪时通知请求任务。 */
  std::optional<std::string> value;       /**< 为空时仍在加载，填入后保留已发布的配置。 */
};

/**
 * @brief 模拟加载配置，取得锁后发布，再唤醒正在等待的请求。
 * @param config 发布者与请求共享的对象，存活到全部任务结束。
 */
faio::task<void> publish(SharedConfig& config) {
  co_await faio::time::sleep(5ms);
  {
    auto guard = co_await config.mutex.scoped_lock();
    config.value.emplace("catalog");
  } /**< 守卫先释放锁，收到通知的任务随后就能取得锁。 */
  config.changed.notify_all();
}

/**
 * @brief 等待配置就绪，复制受保护的值，再取得并发许可并完成请求。
 * @param config 保存共享配置的对象。
 * @param slots 请求共用的并发配额。
 * @param request_id 用于生成结果的请求编号。
 * @return 当前请求读取到的配置与编号。
 */
faio::task<std::string> request(SharedConfig& config,
                                faio::sync::semaphore& slots,
                                int request_id) {
  std::string name;
  {
    auto guard = co_await config.mutex.scoped_lock();
    /** @brief 等待时释放 mutex，返回时重新持有。发布先发生时，谓词会立即满足。 */
    co_await config.changed.wait(config.mutex, [&] { return config.value.has_value(); });
    name = *config.value;
  }

  /** @brief 异步等待期间只持有并发许可。任务退出时，permit 自动归还配额。 */
  auto permit = co_await slots.acquire_permit();
  co_await faio::time::sleep(10ms);
  co_return name + "-" + std::to_string(request_id);
}

/** @brief 并发提交一个发布任务和三个请求，最多允许两个请求同时执行。 */
faio::task<void> demonstrate() {
  SharedConfig config;
  faio::sync::semaphore slots{2};
  const auto [first, second, third, published] = co_await faio::join(request(config, slots, 1),
                                                                     request(config, slots, 2),
                                                                     request(config, slots, 3),
                                                                     publish(config));
  (void)published;
  if (first != "catalog-1" || second != "catalog-2" || third != "catalog-3")
    throw std::runtime_error("请求读取的配置错误");
  std::cout << first << ", " << second << ", " << third << '\n';
}

}  // namespace

/** @brief 等待示例完成，发生错误时打印原因并返回非零状态。 */
int main() {
  try {
    faio::block_on(demonstrate());
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "共享状态示例失败：" << error.what() << '\n';
    return 1;
  }
}
