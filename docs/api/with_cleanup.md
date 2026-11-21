# with_cleanup

公共头文件为 `<faio/coroutine/lifecycle.hpp>`。

```cpp
auto with_cleanup(body_factory, cleanup_factory)
```

## 语义与限制

在任务体结束后用独立停止上下文运行清理并等待其退出。cleanup_error 保留双失败。

## 成本

组合帧与独立停止连接。业务资源必须活到清理结束。

完整程序见 [协程生命周期示例](../../examples/coroutine/lifecycle.cpp)。
