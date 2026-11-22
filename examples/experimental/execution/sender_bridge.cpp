/**
 * @file examples/experimental/execution/sender_bridge.cpp
 * @brief 将 faio 协程接入 stdexec Sender 图，再从协程等待结算结果。
 * @details 先将一项基础报价转换为 Sender，再用 stdexec::then 计算总价。
 * 异步入口提交整个图，在等待期间让出执行权，完成后领取结果。
 */
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <utility>

#include <stdexec/execution.hpp>

#if defined(FAIO_USE_MODULES) && FAIO_USE_MODULES
#include <meta>
import faio.experimental;
#include <faio-module/entry.hpp>
#else
#include <faio/faio.hpp>
#include <faio/experimental/execution.h>
#include <faio/experimental/async_main.h>
#endif

namespace {
/**
 * @brief 模拟需要异步等待的基础报价查询。
 * @return 让出执行权一次后交付的基础价格。
 */
faio::task<int> base_quote() {
  co_await faio::this_coro::yield();
  co_return 40;
}

/**
 * @brief 在 Sender 图中对成功交付的基础报价加收固定服务费。
 * @param price 上一个步骤交付的基础价格。
 * @return 含服务费的总价。
 */
int add_service_fee(int price) noexcept {
  return price + 2;
}
}  // namespace

/**
 * @brief 构建并提交报价图，检查异步领取的结算结果。
 * @return 计算结果为 42 时返回成功退出码，图取消或结果不符时返回失败退出码。
 */
[[= faio::experimental::main(app)]] faio::task<int> app(int, char**) {
  /** @brief as_sender 接管惰性任务，then 声明成功路径的变换。提交前报价任务尚未启动。 */
  auto pricing = stdexec::then(faio::experimental::as_sender(base_quote()), add_service_fee);
  auto pending = faio::experimental::spawn_sender(std::move(pricing));

  /** @brief 句柄的结果只能领取一次：成功时返回值元组，协作停止时返回空值，异常在等待时向上传播。 */
  const auto result = co_await std::move(pending);
  if (!result) {
    std::fputs("报价图已停止\n", stderr);
    co_return EXIT_FAILURE;
  }
  const auto [total] = *result;
  if (total != 42) {
    std::fputs("结算结果不符\n", stderr);
    co_return EXIT_FAILURE;
  }
  std::printf("Sender 图交付含服务费报价 %d\n", total);
  co_return EXIT_SUCCESS;
}
