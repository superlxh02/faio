# serial_domain

多项操作要互斥执行，而且互斥范围跨越协程挂起点时，用 `serial_domain`。它把完整的用户任务从进入到退出串行化；协程互斥锁只保护一段临界区。先包含 `<faio/coroutine/composition.hpp>`。

```cpp
template <class S = default_scheduler>
class serial_domain {
 public:
  template <class Factory>
  auto run(Factory factory);

  task<void, S> close();
};
```

`run` 接收一个无参工厂，工厂返回 `task<R, S>`；`run` 返回同一类型的任务。等待这个任务时先取得域，再调用工厂执行任务体。第二个并发的 `run` 会等第一个任务的协程完全退出，包括期间的每次挂起。返回值、异常和取消沿原任务协议交付。

同一个域对象可以被多个任务共享；域本身不可复制，需要共享时用引用或 `shared_ptr`，并且所有 `run` 都通过同一个域实例进入。

`close()` 关闭接纳并等待所有已进入或已排队的调用退出。关闭后等待新的 `run` 结果会抛出 `std::logic_error`。关闭要在域外发起：域内任务如果等待 `close()`，会因为域尚未释放而一直等不到。

域内任务开始时检查停止请求，已经收到请求就抛出 `operation_cancelled`。域内任务不能同步等待同一域里的其他工作，也不能等待排在自身之后的工作，否则会自锁。需要串行化的工作应尽量短，长时间挂起会阻塞后续所有提交。

域对象通过共享控制块持有状态，`run` 在创建时取得共享所有权，因此惰性任务可以晚于外层域对象销毁。代价是每次进入都要经过一次协程互斥锁和一次接纳计数，吞吐按域串行。

完整程序见 [串行域](../../examples/coroutine/serial_domain.cpp)。
