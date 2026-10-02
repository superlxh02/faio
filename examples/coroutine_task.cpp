#include "faio/faio.hpp"
#include "faio/log.hpp"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace {
// 重复使用的小任务单独具名，demo 里直接展示公开接口。
// 调用此函数只创建惰性 task；执行到 co_await 时只挂起协程，不阻塞线程。
faio::task<int> delayed_value(int value, std::chrono::milliseconds delay) {
  co_await faio::time::sleep(delay);
  co_return value;
}

void demo_task_and_block_on() {
  // 无捕获协程 lambda 通过参数把数据复制进协程帧，避免引用临时闭包。
  auto pending = [](int value) -> faio::task<int> { co_return value; }(42);
  // task 为移动独占、惰性的一次性任务；交给 block_on 后不能再使用 pending。
  // block_on 用在普通线程，等待结果；worker 内必须改用 co_await。
  faio::log::logger()->info("block_on(task)：{}", faio::block_on(std::move(pending)));
}

faio::task<void> demo_nested_task() {
  // co_await task 不创建独立并发任务；这里按顺序执行两个孩子。
  const auto first = co_await delayed_value(20, 1ms);
  const auto second = co_await delayed_value(22, 1ms);
  faio::log::logger()->info("顺序 co_await task：{}", first + second);
}

void demo_spawn_from_thread() {
  // spawn 立即提交，返回 join_handle；普通线程可以继续做其他工作。
  auto child = faio::spawn(delayed_value(7, 2ms));
  child.wait(); // 只等待，不领取结果；不能在 worker 上使用。
  faio::log::logger()->info("join_handle.done()：{}", child.done());
  faio::log::logger()->info("join_handle.get()：{}", child.get()); // 结果只能领取一次。
}

faio::task<void> demo_spawn_from_coroutine() {
  auto first = faio::spawn(delayed_value(10, 3ms));
  auto second = faio::spawn(delayed_value(20, 1ms));
  // 两个任务已并发启动；co_await 句柄只挂起当前协程，worker 可以执行其他任务。
  const auto a = co_await first;  // 等待会转出句柄消费权，first 随后为空。
  const auto b = co_await second;
  faio::log::logger()->info("spawn / co_await join_handle：{}, {}", a, b);
}

void demo_spawn_detached() {
  std::atomic<int> completed{0};
  // 这个外层任务只有 demo 自己需要，无捕获 lambda 直接接受参数。
  // block_on 的任务组会排空其中派生的 detached 孩子，completed 的引用始终有效。
  faio::block_on([](std::atomic<int>& count) -> faio::task<void> {
    for (int i = 0; i < 2; ++i) {
      faio::spawn_detached([](std::atomic<int>& target) -> faio::task<void> {
        co_await faio::time::sleep(1ms);
        target.fetch_add(1, std::memory_order_relaxed);
      }(count));
    }
    co_return;
  }(completed));
  // detached 不返回结果句柄，未捕获异常会 terminate，失败应在任务体中处理。
  faio::log::logger()->info("spawn_detached 完成数量：{}", completed.load());
}

faio::task<void> demo_join() {
  // join 本身返回惰性组合任务；co_await 时才并发启动全部分支。
  // tuple 顺序与参数顺序一致，void 分支对应 std::monostate。
  auto [a, b, nothing] = co_await faio::join(
      delayed_value(10, 3ms), delayed_value(20, 1ms),
      []() -> faio::task<void> { co_return; }());
  (void)nothing;
  faio::log::logger()->info("co_await join：{}, {}", a, b);
}

void demo_join_from_thread() {
  // 普通调用 join 只创建 task；同步等待明确写成 block_on(join(...))。
  auto [a, b] = faio::block_on(faio::join(delayed_value(3, 1ms), delayed_value(4, 1ms)));
  faio::log::logger()->info("block_on(join(...))：{}, {}", a, b);
}

faio::task<void> demo_join_all() {
  // 动态数量、相同返回类型用 join_all；结果 vector 保持输入顺序。
  std::vector<faio::task<int>> tasks;
  for (int i = 1; i <= 3; ++i) tasks.push_back(delayed_value(i, 1ms));
  const auto values = co_await faio::join_all(std::move(tasks));
  faio::log::logger()->info("co_await join_all：{}, {}, {}", values[0], values[1], values[2]);
}

faio::task<void> demo_select() {
  std::atomic<bool> cancelled{false};
  // select 并发启动分支，取最先完成者，然后请求其他分支停止并等待它们退出。
  // 慢分支通过参数借用 cancelled，select 排空后才能读取/销毁它。
  auto winner = co_await faio::select(delayed_value(123, 1ms),
      [](std::atomic<bool>& stopped) -> faio::task<std::string> {
        try { co_await faio::time::sleep(100ms); }
        catch (const faio::operation_cancelled&) { stopped.store(true); throw; }
        co_return "慢分支";
      }(cancelled));
  // 分支由下标区分，即使返回类型相同也不会混淆；value 是 variant。
  if (winner.index != 0) throw std::runtime_error("select 示例预期快速分支获胜");
  faio::log::logger()->info("select：index={}，value={}，慢分支已取消={}",
                       winner.index, std::get<0>(winner.value), cancelled.load());
}

void demo_scope() {
  std::atomic<int> completed{0};
  // scope 把 body 闭包按值保存进自己的协程帧，因此这里可以直接捕获 completed。
  // 普通临时的带捕获协程 lambda 没有这层生命周期保证，应使用无捕获+参数。
  auto result = faio::block_on(faio::scope([&completed](faio::scope_context& group) -> faio::task<int> {
    for (int i = 1; i <= 2; ++i) {
      group.spawn([](std::atomic<int>& target, int amount) -> faio::task<void> {
        co_await faio::time::sleep(1ms);
        target.fetch_add(amount, std::memory_order_relaxed);
      }(completed, i));
    }
    co_return 42; // body 返回后 scope 继续等全部孩子；孩子失败会停止兄弟并传播异常。
  }));
  faio::log::logger()->info("scope：body={}，子任务累计={}", result, completed.load());
}

faio::task<void> demo_this_coro() {
  const auto token = co_await faio::this_coro::stop_token();
  const auto scheduler = co_await faio::this_coro::scheduler();
  const auto worker = co_await faio::this_coro::worker_id();
  const auto priority = co_await faio::this_coro::priority();
  faio::log::logger()->info("this_coro：worker={}，scheduler={}，可取消={}，priority={}",
                       worker, static_cast<bool>(scheduler), token.stop_possible(), static_cast<int>(priority));
  co_await faio::this_coro::yield(); // 总是让出，恢复后可能在另一个 worker。
  for (int i = 0; i < 128; ++i) co_await faio::this_coro::yield_if_needed(); // 预算耗尽时让出。
}

void demo_cancellation() {
  auto child = faio::spawn(delayed_value(1, 100ms));
  child.request_stop(); // 协作取消，发请求不等于任务已退出，必须继续观察句柄。
  try { (void)child.get(); }
  catch (const faio::operation_cancelled&) { faio::log::logger()->info("request_stop：任务已取消"); }
}

void demo_exception() {
  try {
    faio::block_on([]() -> faio::task<void> { throw std::runtime_error("示例异常"); co_return; }());
  } catch (const std::runtime_error& error) {
    faio::log::logger()->info("block_on 异常传播：{}", error.what());
  }
  auto child = faio::spawn([]() -> faio::task<int> { throw std::runtime_error("孩子异常"); co_return 0; }());
  try { (void)child.get(); }
  catch (const std::runtime_error& error) { faio::log::logger()->info("join_handle 异常传播：{}", error.what()); }
}
} // namespace

int main() {
  faio::log::logger()->set_level(spdlog::level::info);
  // 必须在首次使用默认 runtime 前配置；普通应用不需要创建或传入 ctx。
  faio::runtime::configure(faio::config_builder{}.set_num_workers(2).build());
  demo_task_and_block_on();
  faio::block_on(demo_nested_task());
  demo_spawn_from_thread();
  faio::block_on(demo_spawn_from_coroutine());
  demo_spawn_detached();
  faio::block_on(demo_join());
  demo_join_from_thread();
  faio::block_on(demo_join_all());
  faio::block_on(demo_select());
  demo_scope();
  // priority 是元数据；当前调度器不保证优先级抢占。
  faio::block_on(demo_this_coro().with_priority(faio::task_priority::high));
  demo_cancellation();
  demo_exception();
  faio::runtime::shutdown(); // 排空并关闭；默认 runtime 关闭后不能重新启动。
}
