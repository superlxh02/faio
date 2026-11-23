/**
 * @file composition_basics.cpp
 * @brief 并发上篇，展示固定汇合、批量汇合与竞速。
 * @details 三种组合分别对应不同类型的固定任务组、同类型的动态任务组和最先完成的分支。
 */
#include <faio/faio.hpp>

#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
/**
 * @brief 让出执行权后交付供应商报价。
 * @param price 供应商给出的价格。
 * @return 供应商报价。
 */
faio::task<int> quote(int price) {
  co_await faio::this_coro::yield();
  co_return price;
}

/** @brief 返回商品名称，演示异构任务可以一起汇合。 */
faio::task<std::string_view> product_name() {
  co_return "键盘";
}

/**
 * @brief 等备用查询开始后交付主供应商的报价。
 * @param backup_started 备用查询进入时打开的信号，用来确定竞速顺序。
 * @return 本次竞速会选中的报价。
 */
faio::task<int> ready_quote(faio::sync::latch& backup_started) {
  co_await backup_started.wait();
  co_return 42;
}

/** @brief 检查 select 返回前，落选任务已响应停止并完成清理。 */
faio::task<void> show_select() {
  faio::sync::latch backup_started{1};
  faio::sync::latch backup_response{1};
  bool backup_cleaned = false;
  auto backup = faio::with_cleanup(
      [&]() -> faio::task<int> {
        backup_started.count_down();
        /** @brief 备用报价不会就绪，等待会在收到停止请求后结束。 */
        co_await backup_response.wait();
        co_return 38;
      },
      [&](const faio::task_exit& exit) -> faio::task<void> {
        backup_cleaned = exit.status == faio::task_exit::kind::cancelled;
        co_return;
      });
  auto winner = co_await faio::select(ready_quote(backup_started), std::move(backup));
  if (winner.index != 0 || std::get<0>(winner.value) != 42 || !backup_cleaned)
    throw std::logic_error("获胜报价或备用查询的清理状态不正确");
  faio::log::logger()->info("采用首个报价 {}，备用查询已收尾", std::get<0>(winner.value));
}

/** @brief 依次演示三种组合，每组都在退出前等待自己的工作结束。 */
faio::task<void> show_composition() {
  /** @brief join 等全部分支完成，按参数位置交付元组字段。 */
  const auto [name, price] = co_await faio::join(product_name(), quote(42));
  if (name != "键盘" || price != 42)
    throw std::logic_error("固定汇合结果不正确");
  faio::log::logger()->info("{} 的价格为 {}", name, price);

  std::vector<faio::task<int>> quotes;
  quotes.push_back(quote(42));
  quotes.push_back(quote(38));
  /** @brief join_all 接管动态任务列表，结果按输入顺序排列。 */
  const auto prices = co_await faio::join_all(std::move(quotes));
  if (prices.size() != 2 || prices[0] != 42 || prices[1] != 38)
    throw std::logic_error("批量汇合结果不正确");
  faio::log::logger()->info("按输入顺序收集报价：{}、{}", prices[0], prices[1]);

  co_await show_select();
}
}  // namespace

/** @brief 执行组合示例并报告调用失败。 */
int main() {
  try {
    faio::block_on(show_composition());
  } catch (const std::exception& error) {
    faio::log::logger()->error("并发组合失败：{}", error.what());
    return 1;
  }
}
