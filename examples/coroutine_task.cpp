#include "faio/faio.hpp"
#include "fastlog/fastlog.hpp"

// ============================================================================
// 示例1: co_await 协程
// 使用 block_on 阻塞等待子协程完成并获取结果。
// ============================================================================

void demo1(faio::runtime_context &ctx) {
  fastlog::console.info("===== 示例1: co_await 协程 =====");
  auto child_task = []() -> faio::task<std::string> {
    fastlog::console.info("  child task start");
    co_await faio::time::sleep(std::chrono::seconds(1));
    co_return "hello world";
  };

  auto main_task = [&child_task]() -> faio::task<void> {
    auto res = co_await child_task();
    fastlog::console.info("  main_task result: {}", res);
    co_return;
  };
  faio::block_on<void>(ctx, main_task());
}

// ============================================================================
// 示例2: spawn 协程
// 使用 spawn 启动协程不会阻塞，立即返回；使用 block_on 阻塞等待。
// ============================================================================

void demo2(faio::runtime_context &ctx) {
  fastlog::console.info("===== 示例2: spawn 协程 =====");
  auto child_task = []() -> faio::task<void> {
    fastlog::console.info("  child task start");
    co_await faio::time::sleep(std::chrono::seconds(1));
    co_return;
  };

  auto main_task = [&child_task]() -> faio::task<void> {
    fastlog::console.info("  main_task start");
    faio::spawn(child_task());
    co_return;
  };
  faio::block_on<void>(ctx, main_task());
}

// ============================================================================
// 示例3: wait_all 并行执行
// 使用 wait_all 并行执行多个协程，阻塞等待全部完成后返回结果。
// ============================================================================

void demo3(faio::runtime_context &ctx) {
  fastlog::console.info("===== 示例3: wait_all 并行执行 =====");
  auto task1 = []() -> faio::task<std::string> {
    fastlog::console.info("  task1 start");
    co_await faio::time::sleep(std::chrono::seconds(1));
    co_return "hello world";
  };
  auto task2 = []() -> faio::task<int> {
    fastlog::console.info("  task2 start");
    co_await faio::time::sleep(std::chrono::seconds(1));
    co_return 1;
  };
  auto [result1, result2] = faio::wait_all(ctx, task1(), task2());
  fastlog::console.info("  task1 result: {}", result1);
  fastlog::console.info("  task2 result: {}", result2);
}

// ============================================================================
// 示例4: block_on 阻塞等待并返回结果
// ============================================================================

void demo4(faio::runtime_context &ctx) {
  fastlog::console.info("===== 示例4: block_on 返回结果 =====");
  auto main_task = []() -> faio::task<int> {
    fastlog::console.info("  main_task start");
    co_await faio::time::sleep(std::chrono::seconds(1));
    co_return 1;
  };
  auto result = faio::block_on<int>(ctx, main_task());
  fastlog::console.info("  main_task result: {}", result);
}

// ============================================================================
// 示例5: spawn 嵌套与 block_on
// ============================================================================

void demo5(faio::runtime_context &ctx) {
  fastlog::console.info("===== 示例5: spawn 嵌套 =====");
  auto main_task = []() -> faio::task<std::string> {
    fastlog::console.info("  main_task start");
    faio::spawn([]() -> faio::task<void> {
      fastlog::console.info("  main_task child1");
      faio::spawn([]() -> faio::task<void> {
        fastlog::console.info("  main_task child2");
        faio::spawn([]() -> faio::task<void> {
          fastlog::console.info("  main_task child3");
          co_await faio::time::sleep(std::chrono::seconds(1));
          co_return;
        }());
        co_return;
      }());
      co_return;
    }());
    co_return "Hello, World";
  };
  auto result = faio::block_on<std::string>(ctx, main_task());
  fastlog::console.info("  main_task result: {}", result);
}

int main() {
  fastlog::set_consolelog_level(fastlog::LogLevel::Info);
  faio::runtime_context ctx;
  demo1(ctx);
  demo2(ctx);
  demo3(ctx);
  demo4(ctx);
  demo5(ctx);
  fastlog::console.info("===== all coroutine examples done =====");
  return 0;
}