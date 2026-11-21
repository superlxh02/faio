# join_all_limited

公共头文件为 `<faio/coroutine/composition.hpp>`。

```cpp
auto join_all_limited(std::size_t limit, std::vector<task<T, S>> work)
```

## 语义与限制

按输入顺序交付终态。窗口限制已接纳工作数，停止后用 not_started 表示未接纳项。

## 成本

每个窗口许可跨挂起保持，结果存量随输入数增长。

完整程序见 [批处理与任务终态](../../examples/coroutine/batch_results.cpp)。
