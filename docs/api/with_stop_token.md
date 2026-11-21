# with_stop_token

公共头文件为 `<faio/coroutine/lifecycle.hpp>`。

```cpp
task<T, S> with_stop_token(std::stop_token token, task<T, S> work)
```

## 语义与限制

父或显式令牌任一停止都会请求子任务停止。父令牌身份保持原样。

## 成本

局部链接停止源和一个普通组合帧。

完整程序见 [停止与截止](../../examples/coroutine/cancellation_deadline.cpp)。
