# complete_before

公共头文件为 `<faio/coroutine/lifecycle.hpp>`。

```cpp
auto complete_before(task<T, S> work, task<void, S> trigger)
```

## 语义与限制

五种可观察终态。触发器成功表示到期。返回前协作停止并排空落选工作。

## 成本

两个工作根。超时并不等于借用已还，应等组合返回。

完整程序见 [停止与截止](../../examples/coroutine/cancellation_deadline.cpp)。
