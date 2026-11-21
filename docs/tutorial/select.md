# select 与并发组合

前面我们让任务并发跑起来，用 `join` 等它们全部结束。但有时你只关心"谁先完成"，或者想给一组操作加个截止时间。这一章把 `select`、`race_in`、`complete_before` 这些组合子理清楚。

## `select`：谁先完成算谁赢

`select()` 等待多个任务，第一个完成的（无论是成功还是异常）胜出：

```cpp
auto winner = co_await faio::select(ready_quote(), slow_quote());
std::cout << "采用报价 " << std::get<0>(winner.value) << '\n';
```

返回值有两个字段：`index` 是胜出分支在**原始参数**里的位置，`value` 是一个 `variant`，装着胜出者的结果。用 `std::get<index>` 按位置取值。

`select` 有个重要承诺：选出胜者之后，它会请求其余分支停止，并**等它们真正退出**才返回。这保证了返回时没有悬空的任务还在借用你的资源。所以下面这段代码是安全的：

```cpp
auto winner = co_await faio::select(ready_quote(backup_started),
                                    std::move(backup));
// 到这里 backup 已经确认退出了
```

如果落选分支需要自定义清理逻辑，可以用 `with_cleanup()` 包一层，它会收到 `task_exit`，告诉你分支是成功、失败还是被取消。完整例子见 [`examples/coroutine/composition_basics.cpp`](../../examples/coroutine/composition_basics.cpp)。

## `select_success`：要第一个成功的

`select` 把异常也算作完成——如果某个分支先抛异常，它就赢了。很多时候这不是你想要的。你想要的是"忽略失败，取第一个成功的结果"：

```cpp
auto result = co_await faio::select_success(primary(), fallback());
if (result.winner_index) {
  const int price = std::get<*result.winner_index>(*result.value);
}
```

`select_success` 会跳过失败和停止的分支，等第一个成功的。如果全军覆没，`winner_index` 为空，`failures` 里按输入顺序记下每个分支的失败原因，方便诊断。

它和 `select` 一样，选出结果后会停止并排空其余分支。

## `join` 的异常边界

顺便把 `join` 的异常行为说清楚。看这组对照：

```cpp
// join：等所有分支退出，再按参数位置重抛第一个失败
co_await faio::join(task_a(), task_b(), task_c());

// join_settled：不重抛，把每项结果都保留下来
auto [a, b, c] = co_await faio::join_settled(task_a(), task_b(), task_c());
```

`join` 不会因为一个分支失败就取消其他分支——它老老实实等全部退出，然后按参数顺序重抛第一个异常。`join_settled` 则把每个结果包装成 `task_outcome<T>`，保留成功、失败或停止三种终态，由你自己决定怎么处理。

`task_outcome` 的常用方法：

```cpp
outcome.status();       // value / error / stopped
outcome.take_value();   // 取出成功值
outcome.exception();    // 失败的异常指针
outcome.stop_reason();  // 停止原因
```

单任务版本是 `settle()`，动态列表版本是 `join_all_settled()`，限并发版本是 `join_all_limited(limit, tasks)`。

## `race_in`：提前领结果，稍后收尾

`select` 会等所有分支退出才返回。有些场景你希望**先拿到结果**，但让落选分支在后台慢慢清理——只要清理结束前不释放它借用的资源就行。

`race_in` 配合 `task_group` 做这件事：

```cpp
auto summary = co_await faio::task_group([&](faio::task_group_context& owner)
    -> faio::task<int> {
  auto slow = faio::with_cleanup(
      [&]() -> faio::task<int> {
        borrowed = true;
        co_await pending.wait();
        co_return 0;
      },
      [&](const faio::task_exit&) -> faio::task<void> {
        co_await allow_cleanup.wait();
        borrowed = false;
      });

  auto result = co_await faio::race_in(owner, quote(), std::move(slow));
  // 结果已经拿到，但 slow 可能还在清理 borrowed 资源
  co_return std::get<0>(result.outcome).take_value();
});
// 走到这里，组已经排空，borrowed 一定已经归还
```

`race_in` 在第一个终态就绪后就把控制权交回同一词法组内继续执行，落选工作收到协作停止，由任务组的退出负责排空。迟到的异常可以用 `result.loser_error(i)` 查询。完整例子见 [`examples/coroutine/race_strategies.cpp`](../../examples/coroutine/race_strategies.cpp)。

## `complete_before`：加一个截止时间

给工作加截止，用 `complete_before(work, trigger)`。trigger 成功表示到期：

```cpp
auto result = co_await faio::complete_before(fetch_quote(), deadline());
switch (result.status()) {
  case faio::deadline_status::value:
    std::cout << "拿到报价 " << result.take_value() << '\n';
    break;
  case faio::deadline_status::deadline:
    std::cout << "超时了\n";
    break;
  // 还有 work_error / trigger_error / stopped
}
```

它有五种终态：`value`（工作赢）、`work_error`、`trigger_error`、`deadline`（截止触发）、`stopped`。返回前会协作停止并排空落选的那一支，所以不用悬心后台还有东西在跑。

如果一个操作本身支持 `.set_timeout()`，也可以直接挂超时——那更简单。但 `complete_before` 的 trigger 可以是任意任务，灵活性更高，比如"用户点了取消"也能当 trigger。

## 该用哪个

简单的判断顺序：

- 等全部完成、按序收结果 → `join` / `join_all`
- 保留每项成功或失败 → `join_settled` / `settle`
- 第一个完成就算，包括异常 → `select`
- 第一个**成功**才算 → `select_success`
- 先领结果、后台收尾 → `race_in` + `task_group`
- 加截止时间 → `complete_before`，或操作自带的 `set_timeout`

## 运行看看

本章的三个组合子各有一个例子可以单独跑：

```sh
cmake --build build
./build/my_app
```

## 下一步

最后我们看一种不同的模式：数据一项接一项地来，你要一边读一边处理，还可能限制同时处理几项。这是异步流，下一章见[异步流](streams.md)。

---

[上一章：深入 task](task-in-depth.md) · [下一章：异步流 →](streams.md)
