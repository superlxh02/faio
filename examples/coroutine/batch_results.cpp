/**
 * @file batch_results.cpp
 * @brief 保留每项报价的终态，并限制同时发出的查询数。
 * @details 单项查询用 settle 取得终态。固定批次用 join_settled 保留部分成功，
 * 动态批次用 join_all_limited 限制已接纳的任务数。
 */
#include <faio/faio.hpp>

#include <atomic>
#include <cstddef>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using faio::task_outcome_status;

/** @brief 模拟供应商报价。空值表示本次查询失败。 */
faio::task<int> quote(std::optional<int> price) {
  if (!price)
    throw std::runtime_error("供应商暂时无法报价");
  co_return *price;
}

/**
 * @brief 通知调用方已启动，再等待一份尚未就绪的报价。
 * @param started 调用方等待此信号后才发出停止请求。
 * @param pending 模拟报价未到。等待支持协作停止，收到请求后会报告取消。
 */
faio::task<int> pending_quote(faio::sync::latch& started, faio::sync::latch& pending) {
  started.count_down();
  co_await pending.wait();
  co_return 42;
}

/** @brief 分别观察单个任务的值、异常与协作停止。 */
faio::task<void> show_single_results() {
  auto value = co_await faio::settle(quote(42));
  const auto failure = co_await faio::settle(quote(std::nullopt));

  faio::sync::latch started{1};
  faio::sync::latch pending{1};
  auto query = faio::spawn(faio::settle(pending_quote(started, pending)));
  co_await started.wait();
  query.request_stop();
  const auto stopped = co_await query;

  /** @brief take_value 消费成功值。异常和停止都有各自的终态，可留待业务统一处理。 */
  if (value.status() != task_outcome_status::value || value.take_value() != 42
      || failure.status() != task_outcome_status::error || !failure.exception()
      || stopped.status() != task_outcome_status::stopped
      || stopped.stop_reason() != faio::task_stop_reason::requested)
    throw std::logic_error("单项查询的终态与预期不符");
  std::cout << "单项报价：成功、异常、协作停止\n";
}

/** @brief 一项失败后仍取得其余输入的结果，终态按输入位置排列。 */
faio::task<void> show_partial_failure() {
  auto [first, failure, last] =
      co_await faio::join_settled(quote(21), quote(std::nullopt), quote(42));
  if (first.take_value() != 21 || failure.status() != task_outcome_status::error
      || last.take_value() != 42)
    throw std::logic_error("结算结果未保留成功报价及失败位置");
  std::cout << "固定批次：成功、失败、成功\n";
}

/** @brief 用门闩控制首批任务的完成时机，再用原子计数观察各任务尚未完成的时间段。 */
struct QuoteWindow {
  static constexpr std::size_t limit = 2;
  faio::sync::latch filled{static_cast<std::ptrdiff_t>(limit)};
  faio::sync::latch release{1};
  std::atomic<std::size_t> in_flight{};
  std::atomic<bool> exceeded{};
};

/**
 * @brief 发出一项报价查询；首个窗口内的任务等调用方确认后才完成。
 * @param window 跨任务共享的观察状态，存活到批次返回。
 * @param index 输入位置，也用于校验批处理交付的顺序。
 * @return 由输入位置生成的报价。
 */
faio::task<std::size_t> bounded_quote(QuoteWindow& window, std::size_t index) {
  if (window.in_flight.fetch_add(1) >= QuoteWindow::limit)
    window.exceeded = true;
  if (index < QuoteWindow::limit) {
    window.filled.count_down();
    co_await window.release.wait();
  }
  --window.in_flight;
  co_return 20 + index;
}

/** @brief 用固定大小的窗口执行动态批次，验证并发上限及结果的输入顺序。 */
faio::task<void> show_bounded_batch() {
  constexpr std::size_t count = 5;
  QuoteWindow window;
  std::vector<faio::task<std::size_t>> queries;
  for (std::size_t index = 0; index < count; ++index)
    queries.push_back(bounded_quote(window, index));

  auto batch = faio::spawn(faio::join_all_limited(QuoteWindow::limit, std::move(queries)));
  co_await window.filled.wait();
  const bool filled = window.in_flight == QuoteWindow::limit && !batch.done();
  window.release.count_down();
  auto results = co_await batch;

  if (!filled || window.exceeded || results.size() != count)
    throw std::logic_error("批处理的实际窗口或结果数量与预期不符");
  for (std::size_t index = 0; index < count; ++index) {
    if (results[index].take_value() != 20 + index)
      throw std::logic_error("批处理未按输入顺序交付报价");
  }
  std::cout << "动态批次：" << count << " 条报价，在途上限 " << QuoteWindow::limit << '\n';
}

/** @brief 顺序运行三种结果收集方式。 */
faio::task<void> show_batch_results() {
  co_await show_single_results();
  co_await show_partial_failure();
  co_await show_bounded_batch();
}
}  // namespace

/** @brief 等待全部批处理示例，发现结果不符时返回非零值。 */
int main() {
  try {
    faio::block_on(show_batch_results());
  } catch (const std::exception& error) {
    std::cerr << "批处理结果示例失败：" << error.what() << '\n';
    return 1;
  }
}
