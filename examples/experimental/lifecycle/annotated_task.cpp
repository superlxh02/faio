/**
 * @file examples/experimental/lifecycle/annotated_task.cpp
 * @brief 用生命周期注解观察协程首次执行和退出。
 * @details 创建任务时只保存参数。首次等待时才执行前置钩子，任务退出后执行后置钩子，
 * 再将值或原异常交给等待方。
 */
#include <faio/faio.hpp>
#include <faio/experimental/lifecycle.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#include <faio/experimental/async_main.h>

namespace quote_demo {
/** @brief 表示供应商暂时无法给出报价。 */
struct QuoteUnavailable : std::runtime_error {
  /** @brief 保留供等待者读取的错误说明。 */
  QuoteUnavailable() : std::runtime_error("供应商暂时无法报价") {}
};

/** @brief 统计带注解任务的开始次数、成功次数和失败次数。 */
struct QuoteTrace {
  inline static std::atomic<int> started{};   /**< 首次执行的任务数。 */
  inline static std::atomic<int> succeeded{}; /**< 正常交付的任务数。 */
  inline static std::atomic<int> failed{};    /**< 以异常退出的任务数。 */

  /** @brief 任务真正启动时记录一次，单纯构造任务不会进入此钩子。 */
  void before() noexcept { ++started; }

  /**
   * @brief 观察任务退出状态，统计本示例关心的成功和失败。
   * @param exit 借用的任务终态，仅在此调用期间使用。
   * @details 钩子保持 noexcept，通过计数记录状态，由调用方领取结果后统一报告。
   */
  void after(const faio::task_exit& exit) noexcept {
    if (exit.status == faio::task_exit::kind::succeeded)
      ++succeeded;
    if (exit.status == faio::task_exit::kind::failed)
      ++failed;
  }
};

/**
 * @brief 让出一次执行权后读取供应商的报价。
 * @param available 供应商当前是否能报价。
 * @return 等待任务得到的报价。
 * @throws QuoteUnavailable 供应商无法报价，等待者可按原异常类型捕获。
 */
[[= faio::experimental::lifecycle<QuoteTrace>{}]] faio::task<int> quote(bool available) {
  co_await faio::this_coro::yield();
  if (!available)
    throw QuoteUnavailable{};
  co_return 42;
}
}  // namespace quote_demo

/**
 * @brief 创建两项报价并依次检查惰性、成功和失败的观察结果。
 * @return 两次任务都恰好执行一对生命周期钩子时返回成功退出码。
 */
[[= faio::experimental::main(app)]] faio::task<int> app(int, char**) {
  using quote_demo::QuoteTrace;

  auto available = quote_demo::quote(true);
  auto unavailable = quote_demo::quote(false);
  if (QuoteTrace::started.load() != 0 || QuoteTrace::succeeded.load() != 0
      || QuoteTrace::failed.load() != 0) {
    std::fputs("创建任务时不应调用生命周期钩子\n", stderr);
    co_return EXIT_FAILURE;
  }
  std::puts("已创建两项报价，尚未调用钩子");

  const int price = co_await std::move(available);
  if (price != 42 || QuoteTrace::started.load() != 1 || QuoteTrace::succeeded.load() != 1
      || QuoteTrace::failed.load() != 0) {
    std::fputs("成功任务的结果或钩子计数不符\n", stderr);
    co_return EXIT_FAILURE;
  }
  std::printf("成功钩子已记录，取得报价 %d\n", price);

  try {
    (void)co_await std::move(unavailable);
  } catch (const quote_demo::QuoteUnavailable& failure) {
    if (QuoteTrace::started.load() != 2 || QuoteTrace::succeeded.load() != 1
        || QuoteTrace::failed.load() != 1) {
      std::fputs("失败任务的钩子计数不符\n", stderr);
      co_return EXIT_FAILURE;
    }
    std::printf("失败钩子已记录，等待方收到原异常：%s\n", failure.what());
    co_return EXIT_SUCCESS;
  }

  std::fputs("第二项报价应向等待方交付异常\n", stderr);
  co_return EXIT_FAILURE;
}
