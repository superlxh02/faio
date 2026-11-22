/**
 * @file examples/experimental/entry/exit_cleanup.cpp
 * @brief 入口返回时请求停止后台任务，并在退出进程前等待异步清理。
 */
#include <coroutine>
#include <iostream>
#include <memory>
#include <stdexcept>

#if defined(FAIO_USE_MODULES) && FAIO_USE_MODULES
#include <meta>
import faio.experimental;
#include <faio-module/entry.hpp>
#else
#include <faio/faio.hpp>
#include <faio/experimental/async_main.h>
#endif

namespace {
/**
 * @brief 模拟一个持有待清理资源的后台任务。
 * @param started 入口与后台任务各持有一份就绪信号的所有权。
 * @details 工作等待一个尚未到达的事件。收到入口的停止请求后，等待结束，
 * with_cleanup 使用独立的停止上下文执行清理。清理可以挂起并继续运行。
 */
faio::task<void> background(std::shared_ptr<faio::sync::latch> started) {
  faio::sync::latch pending{1};
  try {
    co_await faio::with_cleanup(
        [&]() -> faio::task<void> {
          started->count_down();
          co_await pending.wait();
        },
        [](const faio::task_exit& exit) -> faio::task<void> {
          if (exit.status != faio::task_exit::kind::cancelled)
            throw std::logic_error("后台任务应因入口返回而收到停止请求");

          co_await faio::this_coro::yield();
          std::cout << "后台清理完成\n";
        });
  } catch (const faio::operation_cancelled&) {
    /** @brief 后台任务在此处理预期的协作停止，此时异步清理已经完成。 */
    co_return;
  }
  throw std::logic_error("预期后台等待被取消");
}
}  // namespace

/**
 * @brief 提交后台任务，在它开始工作后返回。
 * @return 交给进程的退出状态。入口默认策略先请求停止，再等派生任务退出。
 * @details 就绪信号保证入口返回时后台任务已经开始。可通过两行输出观察执行顺序。
 */
[[= faio::experimental::main(app)]] faio::task<int> app(int, char**) {
  auto started = std::make_shared<faio::sync::latch>(1);
  faio::spawn_detached(background(started));
  co_await started->wait();
  std::cout << "入口准备返回\n";
  co_return 0;
}
