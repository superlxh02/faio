#ifndef FAIO_DETAIL_COROUTINE_SCHEDULER_HPP
#define FAIO_DETAIL_COROUTINE_SCHEDULER_HPP

#include <concepts>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace faio {
namespace detail {
struct yield_awaiter; // 仅库内真实自让出挂起点可以提供立即归还执行权的提示。
struct yield_if_needed_awaiter; // 同样限定合作预算耗尽后发布自己的续体。
} // namespace detail
// 通用调度协议：成功接受句柄后安排一次恢复，不在 enqueue 内直接 resume。
// 实现须允许外部线程投递；抛异常表示没有接管句柄，便于提交方安全回滚。
// concept 检查接口形状；线程安全、恰好一次调度和借用生命周期属于语义约束。
template <class S>
concept coroutine_scheduler =
    requires(S &scheduler, std::coroutine_handle<> handle) {
      { scheduler.enqueue(handle) } -> std::same_as<void>;
    };

// 所属线程使用的就绪接收端；I/O 和定时器无需知道具体队列布局。
// 此接口仅由所属线程调用，可通过模板内联，不要求跨线程安全。
template <class S>
concept ready_sink = requires(S &sink, std::coroutine_handle<> handle) {
  { sink.enqueue_ready(handle) } -> std::same_as<void>;
};

// 两指针的调度借用引用：不分配内存、不持有对象，也不管理根任务计数。
// 具体调度器必须存活到使用它的协程与等待节点全部结束。
class scheduler_ref {
public:
  // 创建未绑定引用，允许 task 在首次启动时继承调度器。
  scheduler_ref() noexcept = default;

  // 根据具体类型生成唯一静态操作表；禁止借用临时对象和再次擦除 scheduler_ref。
  template <coroutine_scheduler S>
    requires(!std::same_as<std::remove_cvref_t<S>, scheduler_ref>)
  explicit scheduler_ref(S &scheduler) noexcept
      : state_(std::addressof(scheduler)), ops_(&operations_for<S>) {}

  // 查询是否有有效调度目标；构造函数保证指针与操作表同时绑定。
  explicit operator bool() const noexcept { return state_ != nullptr; }

  // 把已挂起的协程交给具体调度器；空引用与提交失败交给调用者处理。
  void schedule(std::coroutine_handle<> handle) const {
    if (!*this)
      throw std::logic_error("没有可用的调度器");
    ops_->enqueue(state_, handle);
  }
  /// @brief 发布 IO 完成；runtime
  /// 可延后批量同伴通知，普通调度器回退原提交入口。
  /// @details enqueue_io 也必须支持外部线程调用，不能无条件访问线程私有队列。
  void schedule_io(std::coroutine_handle<> handle) const {
    if (!*this)
      throw std::logic_error("没有可用的调度器");
    ops_->enqueue_io(state_, handle);
  }

  /** @brief 将主动/预算耗尽的让出续体交给公平入队入口。
   * @details runtime 可用 enqueue_yield 将续体放在 FIFO
   * 尾部，避免马上重占快速槽。 只实现 enqueue
   * 的自定义调度器仍进入原提交入口，concept 和借用合同不变。
   * @throws std::logic_error 空引用没有接管句柄；具体入队失败同样向调用方传播。
   */
  void schedule_yield(std::coroutine_handle<> handle) const {
    if (!*this)
      throw std::logic_error(
          "没有可用的调度器"); // 与 schedule 一样在接管前拒绝空目标。
    ops_->enqueue_yield(state_,
                        handle); // 不在此恢复协程，也不改变其停止/预算上下文。
  }

  // 比较具体实例与操作表，用于判断本地快路径是否属于同一个调度域。
  friend bool operator==(scheduler_ref, scheduler_ref) noexcept = default;

private:
  friend struct detail::yield_awaiter; // 显式自让出：发布以后立即结束
                                       // await_suspend。
  friend struct detail::yield_if_needed_awaiter; // 预算自让出：不从 TLS
                                                 // 猜测当前或父子帧身份。
  /** @brief 库内自让出提示；调用者发布自身句柄后必须立即归还执行权。
   * @details 普通公开 schedule_yield
   * 不采用此提示，保留手工投递另一帧的通知合同。
   *          具体调度器不支持窄能力时，静态适配仍按原 yield/enqueue 路径提交。
   * @throws std::logic_error 空引用未接管句柄；具体入队失败沿原异常路径传播。
   */
  void schedule_cooperative_yield(std::coroutine_handle<> handle) const {
    if (!*this)
      throw std::logic_error(
          "没有可用的调度器"); // 与原公开让出一致，失败不接管续体。
    ops_->enqueue_cooperative_yield(
        state_, handle); // 发布后调用者不再访问可能已恢复的帧。
  }
  // 每种具体调度器共用一张操作表；表本身具有静态生命周期。
  struct operations {
    void (*enqueue)(void *,
                    std::coroutine_handle<>); // 转回具体类型并提交句柄。
    void (*enqueue_io)(void *, std::coroutine_handle<>); // 可选批量完成入口。
    void (*enqueue_yield)(
        void *,
        std::coroutine_handle<>); // 可选公平让出入口；引用仍只有两指针。
    void (*enqueue_cooperative_yield)(
        void *, std::coroutine_handle<>); // 静态表的可选自让出能力。
  };
  // 根据 concept 已验证的类型生成安全转换，用户无法手动拼接指针与表。
  template <coroutine_scheduler S>
  static inline constexpr operations operations_for{
      +[](void *state, std::coroutine_handle<> handle) {
        static_cast<S *>(state)->enqueue(handle);
      },
      +[](void *state, std::coroutine_handle<> handle) {
        if constexpr (requires(S &scheduler) { scheduler.enqueue_io(handle); })
          static_cast<S *>(state)->enqueue_io(handle);
        else
          static_cast<S *>(state)->enqueue(handle);
      },
      +[](void *state, std::coroutine_handle<> handle) {
        // 可选扩展同样要求 void 返回；没有该扩展的旧调度器保持原提交语义。
        if constexpr (requires(S &scheduler) {
                        {
                          scheduler.enqueue_yield(handle)
                        } -> std::same_as<void>;
                      })
          static_cast<S *>(state)->enqueue_yield(handle);
        else
          static_cast<S *>(state)->enqueue(handle);
      },
      +[](void *state, std::coroutine_handle<> handle) {
        // 编译期选择窄能力；旧 custom 先回退公平 yield，再回退其原 enqueue。
        if constexpr (requires(S &scheduler) {
                        {
                          scheduler.enqueue_cooperative_yield(handle)
                        } -> std::same_as<void>;
                      })
          static_cast<S *>(state)->enqueue_cooperative_yield(handle);
        else if constexpr (requires(S &scheduler) {
                             {
                               scheduler.enqueue_yield(handle)
                             } -> std::same_as<void>;
                           })
          static_cast<S *>(state)->enqueue_yield(handle);
        else
          static_cast<S *>(state)->enqueue(handle);
      }};

  void *state_{};           // 借用具体调度器实例，不拥有或释放对象。
  const operations *ops_{}; // 借用与实例类型匹配的静态操作表。
};
} // namespace faio
#endif // FAIO_DETAIL_COROUTINE_SCHEDULER_HPP
