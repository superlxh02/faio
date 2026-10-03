#ifndef FAIO_DETAIL_COROUTINE_ROOT_TASK_HPP
#define FAIO_DETAIL_COROUTINE_ROOT_TASK_HPP

#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/coroutine/task_context.hpp"
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <utility>

namespace faio::detail {
// 调度器拥有的根协程与用户 task 分开管理：根协程结束时自动销毁帧。
// promise_type 必须在协程函数定义处完整，因此仅前置声明 detached_task 不够。
// 调度器接管的根协程，与用户 task 的“父 awaiter 销毁帧”模式分开。
// 提交前本对象拥有帧，提交后帧在 final_suspend 的不挂起路径自动销毁。
class detached_task {
public:
  // 根协程 promise，保存结束时归还计数所需的借用目标，不保存用户结果。
  struct promise_type {
    // 根帧销毁时归还外层 tracker 和运行时计数，提交失败清理也走此入口。
    ~promise_type() {
      // 先归还外层任务组的一票，例如解除 block_on 的排空等待。
      if (scope)
        scope->done();
      // 归还运行时活动根任务计数，使运行时能判断何时可以关闭 worker。
      if (lifetime)
        lifetime.finish_task();
    }
    // 可选外层任务计数器，只借用；计数登记必须与此析构中的 done 配对。
    task_tracker *scope{};
    // 独立的根任务生命周期服务，登记成功后才绑定，析构时归还一票。
    task_lifetime_ref lifetime{};
    // 创建协程时：生成提交前拥有根帧的 detached_task 对象。
    detached_task get_return_object() noexcept {
      // 由 promise 定位根协程帧，此时尚未投递执行。
      return detached_task{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    // 首次挂起时：根包装保持惰性，等 start_detached 登记后才投递。
    std::suspend_always initial_suspend() const noexcept { return {}; }
    // 最终挂起时：不保留根帧，结束后自动销毁并归还任务计数。
    std::suspend_never final_suspend() const noexcept { return {}; }
    // 执行无值 co_return 或运行到末尾时：正常结束，不保存用户返回值。
    void return_void() const noexcept {}
    // 异常离开根协程体时：没有父任务接收异常，明确终止进程。
    void unhandled_exception() const noexcept { std::terminate(); }
  };
  // 根 promise 对应的强类型帧句柄，提交入口通过它设置清理目标。
  using handle_type = std::coroutine_handle<promise_type>;
  // 接管新建的尚未提交根帧，不立即执行。
  explicit detached_task(handle_type h) : handle_(h) {}
  // 移动根帧所有权并把源对象置空，保证仅一个提交前持有者。
  detached_task(detached_task &&other) noexcept
      : handle_(std::exchange(other.handle_, {})) {}
  // 禁止复制根帧句柄，避免重复提交与销毁。
  detached_task(const detached_task &) = delete;
  // 销毁尚未交给调度器的根帧；已 take 转出的帧由执行结束负责销毁。
  ~detached_task() {
    if (handle_)
      handle_.destroy();
  }
  // 向提交入口转出唯一根帧所有权，本对象之后不负责销毁。
  handle_type take() noexcept { return std::exchange(handle_, {}); }
  // 投递前绑定根帧析构时的计数归还目标，必须与调用者的登记配对。
  void set_completion(task_lifetime_ref lifetime,
                      task_tracker *scope = nullptr) noexcept {
    // 保存运行时根任务计数的归还接口。
    handle_.promise().lifetime = lifetime;
    // 保存外层任务组的归还目标，无外层任务组时为空。
    handle_.promise().scope = scope;
  }

private:
  // 提交前独占的根帧句柄；转出后为空，避免对象析构重复清理。
  handle_type handle_;
};

// 登记并立即投递根协程，成功后调度器拥有它的执行生命周期。
// 调用前外层 scope 计数已登记；本函数保证投递失败也通过根帧析构归还。
inline void
start_detached(detached_task root, scheduler_ref scheduler,
               task_tracker *scope = nullptr,
               task_lifetime_ref lifetime = current_task_lifetime()) {
  // 即使没有运行时，调用者已登记的作用域计数也要由帧析构归还。
  // 先绑定清理目标，后续异常销毁 root 时仍能归还外层计数。
  root.set_completion({}, scope);
  // 空运行时无法提交，root 的析构会清理尚未执行的根帧。
  if (!scheduler)
    throw std::logic_error("没有可用的运行时");
  // 投递前给运行时活动根任务增加一票，避免 shutdown 漏掉快速任务。
  lifetime.register_task();
  // 登记成功后才让 promise 负责归还；空调度器失败不会归还未登记的计数。
  root.set_completion(lifetime, scope);
  // 转出根帧所有权，随后成功交给调度器或在失败分支手动清理。
  auto handle = root.take();
  try {
    // 调度器选择本地/全局入队；根帧可立即在其他 worker 开始执行。
    scheduler.schedule(handle);
  } catch (...) {
    // 入队失败时没有执行者接管帧，当前线程负责销毁并归还登记。
    handle.destroy(); // promise 析构会注销根任务与作用域计数。
    // 把入队错误继续交给上层，让上层组合器排空已启动的其他任务。
    throw;
  }
}

// 无结果观察者的根包装，拥有 child，借用 tracker；异常由根 promise
// 终止规则处理。
template <class T>
detached_task spawn_coro(task<T> child, task_tracker *tracker) {
  // 没有 join 接收者的后台任务若抛异常，则终止程序，避免静默丢失。
  // 启动用户 task 前安装任务组，内部 spawn 继承这个归属。
  current_tracker = tracker;
  // 消费并等待用户 task；结果在此丢弃，但异常不能静默丢弃。
  co_await std::move(child);
}

// 立即提交不需要结果句柄的任务，供 spawn_detached 使用。
template <class T>
void start_unobserved(scheduler_ref scheduler, task<T> child,
                      task_tracker *tracker = nullptr,
                      task_lifetime_ref lifetime = current_task_lifetime()) {
  // 先构造包装帧，构造失败时尚未增加外层计数。
  auto root = spawn_coro(std::move(child), tracker);
  // 外层任务组登记一票，根 promise 析构负责配对归还。
  if (tracker)
    tracker->add();
  // 把包装交给运行时登记并入队，无 JoinHandle 共享结果分配。
  start_detached(std::move(root), scheduler, tracker, lifetime);
}
} // namespace faio::detail

#endif
