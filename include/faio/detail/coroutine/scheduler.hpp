#ifndef FAIO_DETAIL_COROUTINE_SCHEDULER_HPP
#define FAIO_DETAIL_COROUTINE_SCHEDULER_HPP

#include <concepts>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace faio {
// 通用调度协议：成功接受句柄后安排一次恢复，不在 enqueue 内直接 resume。
// 实现须允许外部线程投递；抛异常表示没有接管句柄，便于提交方安全回滚。
// concept 检查接口形状；线程安全、恰好一次调度和借用生命周期属于语义约束。
template<class S>
concept coroutine_scheduler = requires(S& scheduler, std::coroutine_handle<> handle) {
  { scheduler.enqueue(handle) } -> std::same_as<void>;
};

// 所属线程使用的就绪接收端；I/O 和定时器无需知道具体队列布局。
// 此接口仅由所属线程调用，可通过模板内联，不要求跨线程安全。
template<class S>
concept ready_sink = requires(S& sink, std::coroutine_handle<> handle) {
  { sink.enqueue_ready(handle) } -> std::same_as<void>;
};

// 两指针的调度借用引用：不分配内存、不持有对象，也不管理根任务计数。
// 具体调度器必须存活到使用它的协程与等待节点全部结束。
class scheduler_ref {
public:
  // 创建未绑定引用，允许 task 在首次启动时继承调度器。
  scheduler_ref() noexcept = default;

  // 根据具体类型生成唯一静态操作表；禁止借用临时对象和再次擦除 scheduler_ref。
  template<coroutine_scheduler S>
    requires (!std::same_as<std::remove_cvref_t<S>, scheduler_ref>)
  explicit scheduler_ref(S& scheduler) noexcept
      : state_(std::addressof(scheduler)), ops_(&operations_for<S>) {}

  // 查询是否有有效调度目标；构造函数保证指针与操作表同时绑定。
  explicit operator bool() const noexcept { return state_ != nullptr; }

  // 把已挂起的协程交给具体调度器；空引用与提交失败交给调用者处理。
  void schedule(std::coroutine_handle<> handle) const {
    if (!*this) throw std::logic_error("没有可用的调度器");
    ops_->enqueue(state_, handle);
  }

  // 比较具体实例与操作表，用于判断本地快路径是否属于同一个调度域。
  friend bool operator==(scheduler_ref, scheduler_ref) noexcept = default;

private:
  // 每种具体调度器共用一张操作表；表本身具有静态生命周期。
  struct operations {
    void (*enqueue)(void*, std::coroutine_handle<>); // 转回具体类型并提交句柄。
  };
  // 根据 concept 已验证的类型生成安全转换，用户无法手动拼接指针与表。
  template<coroutine_scheduler S>
  static inline constexpr operations operations_for{
      +[](void* state, std::coroutine_handle<> handle) {
        static_cast<S*>(state)->enqueue(handle);
      }};

  void* state_{};             // 借用具体调度器实例，不拥有或释放对象。
  const operations* ops_{};  // 借用与实例类型匹配的静态操作表。
};
} // namespace faio
#endif // FAIO_DETAIL_COROUTINE_SCHEDULER_HPP
