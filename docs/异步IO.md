# 异步 IO

---

## 1. 类

| 类 / 结构                     | 所在文件          | 职责与含义                                                                                                                                                                                             |
| ----------------------------- | ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **IOuring**                   | io_uring.hpp      | 封装单个 io_uring 实例：初始化、get_sqe、提交、peek_batch/consume CQE、wait(超时)。每 Worker 一个，通过 thread_local current_uring 访问。                                                              |
| **io_user_data_t**            | io_user_data.hpp  | **每个 IO 请求在 CQE 里找回「是谁」的纽带**：存 handle（要恢复的协程）、result（IO 结果或 errno）、timer_task（超时任务指针）、deadline。prep 后 set_data(sqe, &_user_data)，CQE 里用 user_data 取回。 |
| **io_completion_t**           | io_completion.hpp | 对单个 CQE 的薄封装：expected() 取 cqe->res，data() 把 cqe->user_data 转成 io_user_data_t*。                                                                                                           |
| **IORegistrantAwaiter\<IO\>** | io_registrant.hpp | **Proactor 在协程侧的「精髓」**：把「取 SQE → prep → 设 user_data → 挂起协程 → 提交」统一成一套 awaiter；子类只填 prep 和 await_resume。完成时由 IOEngine::drive 根据 user_data 恢复协程并写 result。  |
| **Timeout\<IO\>**             | timeout.hpp       | 对任意 IORegistrantAwaiter 子类的包装：在 await_suspend 里先向 Timer 注册 deadline 和 user_data，再调原 IO 的 await_suspend；到期则设置 ETIMEDOUT 并 cancel，与正常 CQE 一起在 drive 中恢复。          |
| **Waker**                     | waker.hpp         | eventfd + 在 uring 上挂 read；wake_up() 写 eventfd，Worker 在 wait 时被唤醒；CQE 的 user_data 置 nullptr 以在 drive 中跳过。                                                                           |
| **IOEngine**                  | io_engine.hpp     | 每 Worker 一个：drive() 取 CQE → 写 user_data.result、移除 timer_task、push_back(handle)；再 poll 定时器；start_watch；reset_and_submit。                                                              |

**IORegistrantAwaiter** 统一「取 SQE → prep → set_data(_user_data) → await_suspend 存 handle + submit → 完成时由 drive 写 result 并 push_back(handle)」。**io_user_data_t** 是「请求 ↔ 协程」的唯一桥梁；**Timeout** 在其上叠加定时任务，与正常 CQE 共用同一套 drive 逻辑。**IOEngine::drive** 只认 user_data：取 handle、写 result、可选 remove timer_task、入队，不关心具体是 read 还是 write，实现「完成事件 → 恢复对应协程」的通用路径。

---

## 2. Proactor 与 IORegistrantAwaiter 的角色

**Proactor**：应用提交 IO 请求（提交 SQE），不阻塞；内核完成后产生 CQE，由运行时统一取 CQE 并回调/恢复对应上下文。

在 faio 里：

- **提交**：协程里 `co_await stream.read(buf)`，实际 co_await 的是某个继承自 **IORegistrantAwaiter** 的 awaiter；在 **await_suspend** 里把当前协程的 handle 塞进 **io_user_data_t**，并把该结构指针设到 SQE 的 user_data，然后 submit，协程挂起。
- **完成**：Worker 的 **IOEngine::drive** 里 peek_batch 取 CQE，对每个 CQE 用 user_data 找到对应的 io_user_data_t，把 cqe->res 写入 result，若有 timer_task 则 remove，再把 handle 推回任务队列；该协程在后续某次调度时 resume，在 **await_resume()** 里根据 result 返回 expected\<T\>。

**IORegistrantAwaiter** 把「提交 + 挂起 + 与 user_data 绑定」做成模板，让所有具体 IO（read、write、accept、recv…）共用一个套路，即 Proactor 模式在协程侧的集中体现：**一个类型统一「注册到 uring + 挂起 + 结果回填」**。

---

## 3. IORegistrantAwaiter 源码与逻辑

### 3.1 完整源码（节选）

```cpp
// io_registrant.hpp
template <class IO> class IORegistrantAwaiter {
public:
  template <typename F, typename... Args>
    requires std::is_invocable_v<F, io_uring_sqe *, Args...>
  IORegistrantAwaiter(F &&f, Args &&...args) : _sqe(current_uring->get_sqe()) {
    if (_sqe != nullptr) {
      std::invoke(std::forward<F>(f), _sqe, std::forward<Args>(args)...);
      io_uring_sqe_set_data(_sqe, &_user_data);
    } else {
      _user_data.result = -Error::EmptySqe;
    }
  }

  IORegistrantAwaiter(IORegistrantAwaiter &&other)
      : _user_data(std::move(other._user_data)), _sqe(other._sqe) {
    io_uring_sqe_set_data(_sqe, &this->_user_data);
    other._sqe = nullptr;
  }

  bool await_ready() const noexcept { return _sqe == nullptr; }

  void await_suspend(std::coroutine_handle<> handle) {
    _user_data.handle = std::move(handle);
    io::detail::current_uring->submit();
  }

  auto set_timeout_at(std::chrono::steady_clock::time_point deadline) noexcept {
    _user_data.deadline = deadline;
    return time::detail::Timeout{std::move(*static_cast<IO *>(this))};
  }

  auto set_timeout(std::chrono::milliseconds interval) noexcept {
    return set_timeout_at(std::chrono::steady_clock::now() + interval);
  }

protected:
  io_user_data_t _user_data{};
  io_uring_sqe *_sqe;
};
```

### 3.2 构造函数：注册到 uring（prep + set_data）

- **current_uring->get_sqe()**：从当前 Worker 的 io_uring 取一个 SQE；取不到则 _sqe 为 nullptr，后面 await_ready 为 true，不挂起，await_resume 里用 _user_data.result（已设为 EmptySqe）返回错误。
- **std::invoke(f, _sqe, args...)**：调用具体的 **io_uring_prep_***（如 io_uring_prep_read），把 fd、buf、len 等填进 SQE。注意：**liburing 的 prep 会清零 sqe->user_data**，所以不能先 set_data 再 prep。
- **io_uring_sqe_set_data(_sqe, &_user_data)**：prep 之后必须把「本 awaiter 的 _user_data」设进 SQE，这样 CQE 返回时 cqe->user_data 就是指向这块 _user_data 的指针，drive 里才能找到 handle 和写 result。

每个 IO 请求在提交前就已经和一块 io_user_data_t 绑定；这块结构里之后会写入 handle 和 result，是 Proactor 里「请求 ↔ 上下文」的唯一纽带。

### 3.3 await_ready / await_suspend：挂起与提交

- **await_ready()**：若 _sqe == nullptr（取 SQE 失败），返回 true，不挂起；协程直接走到 await_resume，子类可根据 _user_data.result 返回错误。
- **await_suspend(handle)**：把当前协程句柄存进 _user_data.handle；然后 current_uring->submit()。submit() 内部会按配置的 _submit_interval 决定是否立刻 io_uring_submit，总之 SQE 会在某次 submit 时真正提交。协程在此挂起，直到 drive 里把该 handle 推回队列并 resume。

挂起前只做两件事——记住「要恢复谁」（handle）、把请求提交出去（submit）；不在这里等完成，完成在 drive 里统一处理。

### 3.4 移动语义：保证 CQE 仍指向有效 user_data

若 awaiter 被移动（例如 return 一个 Read 对象），_user_data 和 _sqe 会移到新对象；但 SQE 里已经 set_data 指向的是**原** _user_data 的地址，若原对象析构，CQE 回来就会悬空。因此移动后必须 **io_uring_sqe_set_data(_sqe, &this->_user_data)**，让 SQE 指向**当前**对象的 _user_data。这样无论拷贝消除还是移动，最终执行 co_await 的那个对象持有的 _user_data 和 SQE 里的 user_data 一致，CQE 回来时总是有效指针。

### 3.5 超时包装：Timeout\<IO\>

```cpp
// timeout.hpp
template <class T>
  requires std::derived_from<T, io::detail::IORegistrantAwaiter<T>>
class Timeout : public T {
public:
  Timeout(T &&io) : T{std::move(io)} {}

  auto await_suspend(std::coroutine_handle<> handle) -> bool {
    auto *timer_task = runtime::detail::timer::current_timer->add_task(
        this->_user_data.deadline, &this->_user_data);
    this->_user_data.timer_task = timer_task;
    T::await_suspend(handle);
    return true;
  }
};
```

- **set_timeout_at(deadline)** 会把 deadline 写入 _user_data，然后返回 **Timeout\<IO\>(std::move(*this))**，即用同一块 _user_data 和同一个 SQE 包装成 Timeout。
- **Timeout::await_suspend**：先用 current_timer->add_task(deadline, &_user_data) 在时间轮上挂一个「到 deadline 就执行」的任务，返回的 TimerTask* 存进 _user_data.timer_task；再调基类 T::await_suspend(handle) 提交 SQE 并挂起。
- 若**先到期**：Timer::poll 里会执行该 TimerTask，对 io_user_data_t 设 result = -ETIMEDOUT，并提交 io_uring cancel，CQE 仍会回来（或 cancel 的 CQE），drive 里照常根据 user_data 写 result 并 push_back(handle)。
- 若**IO 先完成**：drive 里会 remove_task(user_data->timer_task)，并写 result，协程恢复；定时器侧之后不会再动该 user_data。

**IORegistrantAwaiter + Timeout** 覆盖「无超时 / 有超时」两种 Proactor 路径，且共用同一套 CQE 处理逻辑。

---

## 4. 完成侧：io_user_data_t 与 IOEngine::drive

### 4.1 io_user_data_t

```cpp
// io_user_data.hpp
struct io_user_data_t {
  std::coroutine_handle<> handle{nullptr};
  int result;
  faio::runtime::detail::timer::TimerTask *timer_task{nullptr};
  std::chrono::steady_clock::time_point deadline;
};
```

- **handle**：await_suspend 里写入，drive 里用其 push_back 到任务队列，恢复协程。
- **result**：drive 里用 completions[i].expected()（即 cqe->res）写入；超时路径由 TimerTask::execute 写 -ETIMEDOUT。
- **timer_task**：Timeout 时由 Timer 持有；drive 里若非空则先 remove_task，避免定时器再触发；超时分支在 execute 里置 nullptr 并提交 cancel。

### 4.2 IOEngine::drive 中如何「完成 → 恢复」

```cpp
// io_engine.hpp 节选
auto completed_count = engine._uring.peek_batch(completions);
for (auto i = 0; i < completed_count; i++) {
  auto user_data = completions[i].data();
  if (user_data == nullptr) { continue; }   // waker 的 eventfd
  if (user_data->timer_task != nullptr) {
    engine._timer.remove_task(user_data->timer_task);
  }
  user_data->result = completions[i].expected();
  local_queue.push_back(user_data->handle, global_queue);
}
engine._uring.consume(completed_count);
// ... timer.poll, waker.start_watch, reset_and_submit
```

- **data()**：从 CQE 的 user_data 得到 io_user_data_t*，即 IORegistrantAwaiter 里绑定的那块。
- **timer_task**：若存在说明是带超时的 IO，完成时要把定时器任务移除，避免重复处理。
- **result**：把本次 IO 的结果（或 errno）写回，await_resume() 里子类会读 _user_data.result 转成 expected\<T\>。
- **push_back(user_data->handle)**：把当时在 await_suspend 里存进去的协程句柄重新入队，相当于「完成回调」：不直接 resume，而是交给调度器，保持 Worker 间负载一致。

整条链：**IORegistrantAwaiter 在 await_suspend 里绑定 handle + 提交 SQE → drive 里用 user_data 取 handle、写 result、入队**，即 Proactor 在 faio 中的完整实现。

---

## 5. 子类示例：Read

```cpp
// read.hpp
class Read : public IORegistrantAwaiter<Read> {
public:
  Read(int fd, void *buf, std::size_t nbytes, uint64_t offset)
      : Base{io_uring_prep_read, fd, buf, nbytes, offset} {}

  auto await_resume() const noexcept -> expected<std::size_t> {
    if (this->_user_data.result >= 0)
      return static_cast<std::size_t>(this->_user_data.result);
    return ::std::unexpected{make_error(-this->_user_data.result)};
  }
};
```

构造时只多传一个 **io_uring_prep_read** 和参数，基类负责 get_sqe、invoke(prep)、set_data。**await_resume()** 只根据 _user_data.result 解释成「成功则字节数，失败则 error」，其余（挂起、提交、完成时入队）全部在 IORegistrantAwaiter + drive 里完成。
