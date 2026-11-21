# task_group

公共头文件为 `<faio/coroutine/composition.hpp>`。

```cpp
task<group_result<T>, S> task_group(group_options options, body)
```

## 语义与限制

词法接纳，关闭后排空。丢弃句柄不取消工作。上下文和子工作接纳权限只在主体有效。

## 成本

每个被观察的孩子使用独立结果状态。

完整程序见 [并发下](../../examples/coroutine/structured_concurrency.cpp)。
