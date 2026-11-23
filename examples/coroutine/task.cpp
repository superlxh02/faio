/**
 * @file task.cpp
 * @brief 协程任务的创建、直接等待与独立启动。
 * @details 先观察惰性创建，再独立提交两个报价任务，并通过句柄接收失败。
 */
#include <faio/faio.hpp>

#include <exception>
#include <stdexcept>
#include <utility>

namespace {
/**
 * @brief 记录报价任务开始执行的时机，主动让出执行权后交付结果。
 * @param started 调用方借出的标志，存活到直接等待结束。
 * @return 报价。
 */
faio::task<int> direct_quote(bool& started) {
  started = true;
  co_await faio::this_coro::yield();
  co_return 21;
}

/**
 * @brief 校验报价后让出执行权，使其他已提交的任务也能推进。
 * @param price 查询取得的金额，负数表示数据错误。
 * @return 有效报价。
 * @throws std::invalid_argument 报价为负数，异常由等待方收到。
 */
faio::task<int> quote(int price) {
  if (price < 0)
    throw std::invalid_argument("报价为负数");
  co_await faio::this_coro::yield();
  co_return price;
}

/** @brief 检查惰性、独立启动和句柄的异常交付。 */
faio::task<void> show_tasks() {
  bool started = false;
  auto pending = direct_quote(started);
  if (started)
    throw std::logic_error("创建任务时不应执行任务体");

  /** @brief 具名 task 通过移动转交唯一的等待权，父协程在取到报价后继续执行。 */
  const int unit_price = co_await std::move(pending);
  if (!started || unit_price != 21)
    throw std::logic_error("直接等待未交付预期报价");
  faio::log::logger()->info("单价 {}", unit_price);

  /** @brief 两次 spawn 都先提交任务。随后按顺序领取结果，两个查询已经可以并发推进。 */
  auto first = faio::spawn(quote(20));
  auto second = faio::spawn(quote(22));
  const int first_quote = co_await first;
  const int second_quote = co_await second;
  if (first_quote + second_quote != 42)
    throw std::logic_error("汇总报价错误");
  faio::log::logger()->info("总价 {}", first_quote + second_quote);

  /** @brief 异常保存在任务结果中，调用方等待句柄时可以按原类型捕获。 */
  auto invalid_quote = faio::spawn(quote(-1));
  try {
    (void)co_await invalid_quote;
  } catch (const std::invalid_argument& error) {
    faio::log::logger()->info("等待方收到错误：{}", error.what());
    co_return;
  }
  throw std::logic_error("无效报价本应产生异常");
}
}  // namespace

/**
 * @brief 配置默认运行时，再从普通 main 等待示例协程完成。
 * @details 配置必须发生在第一次 block_on 或 spawn 之前。本例选择多线程调度，
 * 由两个工作线程推进协程。
 */
int main() {
  try {
    faio::runtime::configure(faio::config_builder{}
                                 .set_mode(faio::runtime::mode::multi_thread)
                                 .set_num_workers(2)
                                 .build());
    faio::block_on(show_tasks());
  } catch (const std::exception& error) {
    faio::log::logger()->error("任务示例失败：{}", error.what());
    return 1;
  }
}
