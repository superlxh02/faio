/**
 * @file cancellation_deadline.cpp
 * @brief 连接外部停止令牌，演示工作完成和截止触发两条路径。
 * @details 用公开的协程门闩接口控制先后顺序，便于观察停止请求、清理和结果交付。
 */
#include <faio/faio.hpp>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <utility>

namespace {
using namespace std::chrono_literals;

/**
 * @brief 发布工作实际收到的停止令牌，再挂起等待。
 * @param observed 保存本任务看到的令牌，调用方在 entered 归零后读取。
 * @param entered 工作开始的信号。
 * @param pending 保持未满足的等待条件，收到停止请求后等待抛出 operation_cancelled。
 */
faio::task<void> wait_for_stop(std::stop_token& observed,
                               faio::sync::latch& entered,
                               faio::sync::latch& pending) {
  observed = co_await faio::this_coro::stop_token();
  entered.count_down();
  co_await pending.wait();
}

/** @brief 用应用层持有的停止源取消一项已经开始的异步工作。 */
faio::task<void> show_external_stop() {
  std::stop_source external;
  std::stop_token observed;
  faio::sync::latch entered{1};
  faio::sync::latch pending{1};
  auto job = faio::spawn(
      faio::with_stop_token(external.get_token(), wait_for_stop(observed, entered, pending)));

  co_await entered.wait();
  external.request_stop();
  /** @brief 链接后的令牌将停止请求传给工作任务，提交句柄保留自身的停止状态。 */
  const bool child_received_stop = observed.stop_requested() && !job.stop_token().stop_requested();
  try {
    co_await job;
  } catch (const faio::operation_cancelled&) {
    if (!child_received_stop)
      throw std::logic_error("外部令牌应只停止绑定它的工作");
    faio::log::logger()->info("外部停止源已请求停止，等待任务句柄取得取消终态");
    co_return;
  }
  throw std::logic_error("预期取得取消终态");
}

/** @brief 模拟已经可以完成的工作。 */
faio::task<int> ready_value() {
  co_return 42;
}

/**
 * @brief 确认工作开始后计时，计时结束即触发截止。
 * @param started 计时器启动前要等待的信号。
 */
faio::task<void> wait_for_deadline(faio::sync::latch& started) {
  co_await started.wait();
  co_await faio::time::sleep(20ms);
}

/** @brief 截止信号未触发，工作先发布成功结果。 */
faio::task<void> show_work_wins() {
  faio::sync::latch untriggered{1};
  /** @brief 此处始终不给截止任务启动信号，工作分支的胜负不取决于线程调度速度。 */
  auto result = co_await faio::complete_before(ready_value(), wait_for_deadline(untriggered));
  if (result.status() != faio::deadline_status::value || result.take_value() != 42)
    throw std::logic_error("预期工作先完成并交付 42");
  faio::log::logger()->info("工作先完成：取得 42，截止等待也已退出");
}

/** @brief 截止先触发，等落选任务结束资源借用后再交付截止状态。 */
faio::task<void> show_deadline_wins() {
  faio::sync::latch work_started{1};
  faio::sync::latch work_pending{1};
  faio::sync::latch cleanup_started{1};
  faio::sync::latch allow_cleanup{1};
  std::string_view buffer_use;
  bool work_cancelled = false;

  auto work = faio::with_cleanup(
      [&]() -> faio::task<int> {
        buffer_use = "正在处理数据";
        work_started.count_down();
        co_await work_pending.wait();
        co_return 42;
      },
      [&](const faio::task_exit& exit) -> faio::task<void> {
        work_cancelled = exit.status == faio::task_exit::kind::cancelled;
        cleanup_started.count_down();
        co_await allow_cleanup.wait();
        buffer_use = {};
      });
  auto operation =
      faio::spawn(faio::complete_before(std::move(work), wait_for_deadline(work_started)));

  co_await cleanup_started.wait();
  /** @brief 工作任务已响应停止，触发任务也已报告到期；组合操作仍需等待清理完成、资源借用结束。 */
  const bool waiting_for_cleanup = work_cancelled && !operation.done() && !buffer_use.empty();
  allow_cleanup.count_down();
  auto result = co_await operation;
  if (!waiting_for_cleanup || result.status() != faio::deadline_status::deadline
      || !buffer_use.empty()) {
    throw std::logic_error("预期截止先触发，清理结束后才交付结果");
  }
  faio::log::logger()->info("截止先触发：已收束落选工作，可以复用缓冲");
}

/** @brief 顺序运行外部停止、工作先完成和截止先触发三个场景。 */
faio::task<void> demonstrate() {
  co_await show_external_stop();
  co_await show_work_wins();
  co_await show_deadline_wins();
}
}  // namespace

/** @brief 等待全部场景完成，失败时打印原因并返回非零状态。 */
int main() {
  try {
    faio::block_on(demonstrate());
  } catch (const std::exception& error) {
    faio::log::logger()->error("停止与截止示例失败：{}", error.what());
    return 1;
  }
}
