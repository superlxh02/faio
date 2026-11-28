/**
 * @file race_strategies.cpp
 * @brief 选择首个成功结果，或在任务组内提前领取首个终态。
 * @details 用两个报价场景展示竞速条件和落选分支的收尾责任。
 */
#include <faio/faio.hpp>

#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

namespace {
/** @brief 模拟主供应商拒绝报价。 */
faio::task<int> unavailable_quote() {
  co_await faio::this_coro::yield();
  throw std::runtime_error("主供应商无法报价");
}

/**
 * @brief 在指定事件发生后交付一份可用报价。
 * @param ready 允许返回报价的一次性信号。
 * @return 该供应商给出的报价。
 */
faio::task<int> quote_after(faio::sync::latch& ready) {
  co_await ready.wait();
  co_return 42;
}

/** @brief 主供应商先失败，备用供应商仍能交付首个成功结果。 */
faio::task<void> show_first_success() {
  faio::sync::latch primary_finished{1};
  bool primary_failed = false;
  auto primary = unavailable_quote().with_lifecycle(
      [] {},
      [&](const faio::task_exit& exit) noexcept {
        /** @brief 主查询实际退出后才允许备用查询返回，结果与调度先后无关。 */
        primary_failed = exit.status == faio::task_exit::kind::FAILED;
        primary_finished.count_down();
      });
  auto result = co_await faio::select_success(std::move(primary), quote_after(primary_finished));
  /** @brief 分支类型都为 int，按原输入的下标从 variant 取值。 */
  if (!primary_failed || result.winner_index != 1 || !result.value
      || std::get<1>(*result.value) != 42) {
    throw std::logic_error("预期主查询失败后取得备用报价");
  }
  faio::log::logger()->info("select_success：主查询已失败，备用报价为 42");
}

/** @brief 在竞速返回后处理报价，等待整个任务组退出后再复用借用资源。 */
faio::task<void> show_early_result() {
  faio::sync::latch slow_started{1};
  faio::sync::latch slow_pending{1};
  faio::sync::latch result_seen{1};
  faio::sync::latch allow_cleanup{1};
  bool borrowed = false;

  auto group =
      faio::spawn(faio::task_group([&](faio::task_group_context& owner) -> faio::task<int> {
        auto slow = faio::with_cleanup(
            [&]() -> faio::task<int> {
              borrowed = true;
              slow_started.count_down();
              /** @brief 只有收到停止请求才结束这次等待，慢查询不会先交付结果。 */
              co_await slow_pending.wait();
              co_return 0;
            },
            [&](const faio::task_exit&) -> faio::task<void> {
              /** @brief 清理可以挂起，直到调用方允许释放借用。 */
              co_await allow_cleanup.wait();
              borrowed = false;
            });
        auto result = co_await faio::race_in(owner, quote_after(slow_started), std::move(slow));
        const int price = result.index == 0 ? std::get<0>(result.outcome).take_value() : 0;
        faio::log::logger()->info("race_in：先领取报价 {}，慢查询仍在收尾", price);
        result_seen.count_down();
        co_return price;
      }));

  co_await result_seen.wait();
  /** @brief 通过 slow_started 发布首次写入的借用状态；再次写入前需等待 allow_cleanup 放行。 */
  const bool still_borrowed = borrowed && !group.done();
  allow_cleanup.count_down();
  auto summary = co_await group;
  if (!still_borrowed || borrowed || summary.body.take_value() != 42) {
    throw std::logic_error("预期先交付报价，在任务组退出前归还借用");
  }
  faio::log::logger()->info("task_group：落选分支已经退出，可以复用借出的资源");
}

/** @brief 依次演示首个成功和组内提前领取。 */
faio::task<void> demonstrate() {
  co_await show_first_success();
  co_await show_early_result();
}
}  // namespace

/** @brief 等待竞速示例完成，失败时打印原因并返回非零状态。 */
int main() {
  try {
    faio::block_on(demonstrate());
  } catch (const std::exception& error) {
    faio::log::logger()->error("竞速示例失败：{}", error.what());
    return 1;
  }
}
