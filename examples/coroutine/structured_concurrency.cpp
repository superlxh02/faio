/**
 * @file structured_concurrency.cpp
 * @brief 并发下篇：演示作用域如何等待任务退出，以及结果共享和组内失败传播。
 * @details 以组合报价为例，分别观察无句柄工作的退出、共享税额与供应商故障后的停止。
 */
#include <faio/faio.hpp>

#include <exception>
#include <memory>
#include <stdexcept>

namespace {
/**
 * @brief 让出一次执行权，再把报价写入专属结果槽。
 * @param price 供应商给出的报价。
 * @param result 调用方借出的结果槽，存活到作用域内的任务全部退出后。
 */
faio::task<void> publish_quote(int price, int& result) {
  co_await faio::this_coro::yield();
  result = price;
}

/** @brief 等待方和分离任务共享的通知状态。 */
struct QuoteNotice {
  faio::sync::latch ready{1}; /**< 发布报价后打开，等待方随后可以读取。 */
  int price{};                /**< 由分离任务写入的报价。 */
};

/**
 * @brief 通过信号发布一条缓存报价。
 * @param notice 按值传入的共享所有权，保证分离任务完成发布前通知对象存活。
 */
faio::task<void> post_cached_quote(std::shared_ptr<QuoteNotice> notice) {
  notice->price = 22;
  notice->ready.count_down();
  co_return;
}

/** @brief 对照作用域自动等待与分离任务显式通知这两种方式。 */
faio::task<void> show_scope() {
  int current_quote = 0;
  auto cached_quote = std::make_shared<QuoteNotice>();
  co_await faio::scope([&](faio::scope_context& owner) -> faio::task<void> {
    owner.spawn(publish_quote(20, current_quote));
    /** @brief detached 没有结果句柄。需要观察通知时，使用业务信号显式等待。 */
    faio::spawn_detached(post_cached_quote(cached_quote));
    co_await cached_quote->ready.wait();
  });
  /** @brief current_quote 由 owner.spawn 接纳，scope 在返回前等它退出。 */
  if (current_quote != 20 || cached_quote->price != 22)
    throw std::logic_error("自动收束或显式通知未交付报价");
  faio::log::logger()->info("作用域已收束，分离任务的通知已收到");
}

/** @brief 加载由多个子任务读取的一份配置。 */
faio::task<std::unique_ptr<int>> load_tax() {
  co_return std::make_unique<int>(2);
}

/**
 * @brief 通过共享任务观察税额，再计算含税价格。
 * @param net_price 未税价格。
 * @param tax 独立的共享句柄副本。观察结果的句柄保证结果对象在读取期间存活。
 * @return 含税价格。
 */
faio::task<int> add_tax(int net_price, faio::shared_task<std::unique_ptr<int>> tax) {
  const auto observed = co_await tax.observe_uncancellable();
  co_return net_price + *observed.value();
}

/** @brief 税额只加载一次，两个子任务各自取得其只读结果。 */
faio::task<void> show_shared_result() {
  auto summary = co_await faio::task_group([](faio::task_group_context& group) -> faio::task<int> {
    const auto tax = group.spawn_shared_uncancellable(load_tax());
    auto first = group.spawn_observed(add_tax(18, tax));
    auto second = group.spawn_observed(add_tax(20, tax));
    /** @brief 领取子任务结果。任务组会等待其接纳的所有子任务退出后，再返回汇总结果。 */
    const int first_price = (co_await first).value_or_throw();
    const int second_price = (co_await second).value_or_throw();
    co_return first_price + second_price;
  });
  const int group_total = summary.body.value_or_throw();
  if (group_total != 42)
    throw std::logic_error("含税合计不符合预期");
  faio::log::logger()->info("共享税额后合计 {}", group_total);
}

/**
 * @brief 等待尚未就绪的报价，留出机会让同组故障触发停止。
 * @param started 用于保证这个任务已开始执行的信号。
 */
faio::task<void> wait_for_supplier(faio::sync::latch& started) {
  faio::sync::latch response{1};
  started.count_down();
  co_await response.wait();
}

/** @brief 模拟一个失败的供应商请求。 */
faio::task<int> request_unavailable_supplier() {
  co_await faio::this_coro::yield();
  throw std::runtime_error("供应商不可用");
}

/** @brief 观察失败结果，以及兄弟任务在 fail-fast 策略下的停止行为。 */
faio::task<void> show_group_failure() {
  using faio::task_outcome_status;
  faio::sync::latch peer_started{1};
  auto summary =
      co_await faio::task_group([&](faio::task_group_context& group) -> faio::task<void> {
        auto peer = group.spawn_observed(wait_for_supplier(peer_started));
        co_await peer_started.wait();
        auto failing = group.spawn_observed(request_unavailable_supplier());
        const auto failure = co_await failing;
        if (failure.status() != task_outcome_status::error || !group.stop_token().stop_requested())
          throw std::logic_error("失败应请求同组停止");
        if ((co_await peer).status() != task_outcome_status::stopped)
          throw std::logic_error("同组等待中的请求应观察到停止");
      });
  summary.body.value_or_throw();
  /** @brief 组按接纳顺序汇报终态，等待句柄并不会从摘要中删除该子项。 */
  if (summary.children.size() != 2 || summary.children[0].status != task_outcome_status::stopped
      || summary.children[1].status != task_outcome_status::error)
    throw std::logic_error("任务组摘要缺少预期的子任务终态");
  faio::log::logger()->info("组内失败已记录，兄弟任务已退出");
}
}  // namespace

/** @brief 依次演示作用域、共享结果及组内失败，并把异常转为退出状态。 */
int main() {
  try {
    faio::block_on(show_scope());
    faio::block_on(show_shared_result());
    faio::block_on(show_group_failure());
  } catch (const std::exception& error) {
    faio::log::logger()->error("结构化并发示例失败：{}", error.what());
    return 1;
  }
}
