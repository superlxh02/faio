# 单元测试

异步代码的测试有两个难点：怎么驱动协程，以及怎么避免时序导致随机失败。这一篇讲 faio 项目的测试组织方式，以及写稳定测试的一些做法。

## 测试怎么组织

faio 的测试用 GoogleTest，但不用自定义 main——测试程序链接 `GTest::gtest_main`，main 由框架提供：

```cmake
add_executable(faio_unit_tests test_coroutine.cpp test_sync.cpp ...)
target_link_libraries(faio_unit_tests PRIVATE faio::faio GTest::gtest_main)
add_test(NAME faio_unit_tests COMMAND faio_unit_tests)
```

测试文件里也没有 fixture（`TEST_F`），而是用普通的函数式组织：

- 一个匿名 namespace 包住测试辅助代码；
- 若干辅助 `task` 函数，在里面写实际的异步逻辑和断言；
- 一个 `TEST(...)` 用运行时驱动这些 task。

## 用运行时驱动测试

有两种驱动方式。测试里更常见的是**运行时实例**：

```cpp
TEST(CoroutineAsyncSequence, BoundedMoveOnlyAndErrors) {
  faio::runtime::current_thread_runtime runtime;
  for (const auto n : {1u, 2u, 32u})
    runtime.block_on(limited(n));
  runtime.block_on(fails());
}
```

`current_thread_runtime` 不会创建后台 worker，由调用 `block_on()` 的线程驱动，适合测试——没有线程调度的不确定性。需要测试多线程行为时再换 `multi_thread_runtime` 或用默认运行时。

另一种是直接 `faio::block_on(task)`，用默认运行时。示例程序里都是这种写法。

## 把断言放进 task 里

协程里的断言直接写在 task 函数中，和同步代码一样用 `EXPECT_*`：

```cpp
faio::task<void> limited(std::size_t limit) {
  int live = 0, high = 0, done = 0;
  co_await faio::for_each_concurrent<item>(
      limit,
      [&](faio::stream_producer<item>& sink) -> faio::task<void> {
        for (int i = 0; i < 30; ++i)
          co_await sink.emit(item{live, high});
      },
      [&](item value) -> faio::task<void> {
        EXPECT_TRUE(value.live_);
        co_await faio::this_coro::yield();
        ++done;
      });
  EXPECT_EQ(done, 30);
  EXPECT_EQ(live, 0);
  EXPECT_LE(high, static_cast<int>(limit));
}
```

`TEST` 只负责跑起来。完整例子见 `tests/test_async_sequence.cpp`。

## 测试异常路径

任务抛出的异常会在 `block_on()` 处重抛，可以直接用 `EXPECT_THROW`：

```cpp
faio::task<int> failed_task() {
  co_await faio::this_coro::yield();
  throw std::runtime_error("boom");
}

TEST(Runtime, PropagatesException) {
  faio::runtime::current_thread_runtime runtime;
  EXPECT_THROW(runtime.block_on(failed_task()), std::runtime_error);
}
```

取消也类似——在辅助 task 里 `request_stop()`，然后捕获 `operation_cancelled` 验证。见 `tests/test_stop_boundary.cpp` 和 `tests/test_coroutine_on.cpp`。

## 辅助函数

项目提供了 `tests/test_helpers.hpp`，里面有几个常用的小工具：

```cpp
namespace faio_test {
template <class T>
T take(faio::expected<T> result) {
  if (!result)
    throw std::runtime_error(std::string(result.error().message()));
  return std::move(*result);
}

auto config() { /* 返回配置好的运行时 config */ }
class temporary_directory { /* 临时目录的 RAII 管理 */ };
}
```

`take()` 把 `expected` 转成"成功或抛异常"，在只关心成功值的测试里省去逐项检查。注意这不是库的一部分，只是测试辅助。

这里没有自定义的 `TEST_ASYNC` 之类的宏——faio 的测试不需要它们，普通 `TEST` + `block_on()` 就够了。

## 避免时序不稳定

异步测试最怕"本地通过、CI 偶发失败"。几个实用的做法：

**不要靠 sleep 同步。** 测试里想等某个条件成立，用 `latch`、`condition_variable` 或通道，不要用 `sleep(10ms)` 然后假设任务已经跑完。后者在不同机器上的结果不一样。

**用有确定胜负的构造。** 需要验证"哪个先完成"时，让一个分支永远不就绪（等一个不会触发的 latch），而不是靠两个分支谁跑得快。教程的 `select` 章节用的就是这个办法。

**优先用单线程运行时。** `current_thread_runtime` 消除了线程调度的随机性。只有确实要测多线程行为时才用多线程模式。

**给停止逻辑留出口。** 测试里如果启动了一个无限循环的任务，确保它有办法退出，否则 `block_on()` 永远不会返回。

**给测试设置超时。** CMake 里注册测试时设置了 `TIMEOUT 60`：

```cmake
set_tests_properties(faio_unit_tests PROPERTIES TIMEOUT 60 LABELS unit)
```

这能防止死锁的测试把整个测试套件挂住。

## 运行测试

按[快速开始](../getting-started.md)里的步骤配置 debug 预设，然后：

```sh
cmake --preset linux-main-debug
cmake --build --preset linux-main-debug -j4
ctest --preset linux-main-debug
```

测试文件都放在 `tests/` 下，按主题命名：`test_coroutine.cpp`、`test_sync.cpp`、`test_net.cpp`、`test_runtime.cpp` 等。

## 下一步

专题到这里就结束了。术语上有不清楚的地方，可以查[术语表](../glossary.md)；想找某个接口的准确签名，去[API 介绍](../api/index.md)或[API 查询](../api-reference.md)。

---

[上一章：日志](logging.md) · [专题首页](index.md)
