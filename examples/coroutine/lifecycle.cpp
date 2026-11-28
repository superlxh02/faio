/**
 * @file lifecycle.cpp
 * @brief 使用同步钩子观察任务执行，再通过异步清理结束资源借用。
 * @details 第一段用任务生命周期钩子观察首次执行和结束。
 * 第二段请求停止，等待异步清理结束对记录的借用，再领取取消结果。
 * 用门闩控制各步骤的执行顺序。
 */
#include <faio/faio.hpp>

#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
/** @brief 同一任务上的同步钩子和任务体依次经过的阶段。 */
enum class stage { created, before, body, after };

/**
 * @brief 读取一条记录，验证进入任务体前已经执行前置钩子。
 * @param current 由调用方持有的执行阶段。
 * @return 记录的值。
 */
faio::task<int> read_record(stage& current) {
  if (current != stage::before)
    throw std::logic_error("任务体应在前置钩子之后执行");
  current = stage::body;
  co_return 42;
}

/** @brief 构造带钩子的惰性任务，再确认调用方领取值之前已执行后置钩子。 */
faio::task<void> observe_task() {
  auto current = stage::created;
  auto observed = read_record(current).with_lifecycle(
      [&current] { current = stage::before; },
      [&current](const faio::task_exit& exit) noexcept {
        if (current == stage::body && exit.status == faio::task_exit::kind::SUCCEEDED)
          current = stage::after;
      });
  if (current != stage::created)
    throw std::logic_error("创建任务时应尚未调用钩子");

  const int value = co_await std::move(observed);
  if (value != 42 || current != stage::after)
    throw std::logic_error("任务体应先退出，再运行后置钩子并交付结果");
  faio::log::logger()->info("同步钩子依次观察到任务开始和成功退出，调用方取得 {}", value);
}

/** @brief 请求停止，然后通过任务句柄观察清理完成后的结果。 */
faio::task<void> show_lifecycle() {
  co_await observe_task();

  faio::sync::latch work_started{1};
  faio::sync::latch work_pending{1};
  faio::sync::latch cleanup_started{1};
  faio::sync::latch allow_cleanup{1};
  std::string_view record;

  auto job = faio::spawn(faio::with_cleanup(
      [&]() -> faio::task<void> {
        record = "任务借用的记录";
        work_started.count_down();
        /** @brief 可取消的等待会在收到停止请求时结束，随后由 with_cleanup 执行清理。 */
        co_await work_pending.wait();
      },
      [&](const faio::task_exit& exit) -> faio::task<void> {
        cleanup_started.count_down();
        /** @brief 清理有独立的停止上下文，因此收到原任务的停止请求后仍能异步等待。 */
        co_await allow_cleanup.wait();
        record = {};
        if (exit.status != faio::task_exit::kind::CANCELLED) {
          throw std::logic_error("预期任务收到停止请求后退出");
        }
      }));

  co_await work_started.wait();
  job.request_stop();
  faio::log::logger()->info("已请求停止");

  co_await cleanup_started.wait();
  const bool cleanup_pending = !job.done() && !record.empty();
  faio::log::logger()->info("清理尚未结束，记录仍被借用");
  allow_cleanup.count_down();

  try {
    co_await job;
  } catch (const faio::operation_cancelled&) {
    if (!cleanup_pending || !record.empty()) {
      throw std::logic_error("任务应在归还借用之后交付取消结果");
    }
    faio::log::logger()->info("清理已归还记录，调用方取得取消结果");
    co_return;
  }
  throw std::logic_error("预期领取到取消结果");
}
}  // namespace

/** @brief 等待停止与清理过程结束，并报告意外结果。 */
int main() {
  try {
    faio::block_on(show_lifecycle());
  } catch (const std::exception& error) {
    faio::log::logger()->error("生命周期示例失败：{}", error.what());
    return 1;
  }
}
