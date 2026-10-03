#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_DOMAIN_SCHEDULER_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_DOMAIN_SCHEDULER_HPP

#include "faio/detail/coroutine/execution_thread.hpp"
#include "faio/detail/runtime/common/config.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/ready_queue.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/state_machine.hpp"
#include <cassert>
#include <concepts>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

namespace faio::runtime::detail {
// 唤醒能力的借用引用；调度域选择线程，具体 I/O 后端负责解除底层等待。
// 表只包含冷路径唤醒，不在每次 CQE 处理或本地取任务时执行间接调用。
class worker_waker_ref {
public:
  // 空引用用于尚未注册或已经注销的线程槽位。
  worker_waker_ref() noexcept = default;
  // 只接受不会抛异常的唤醒接口，成功入队后唤醒不能再报告提交失败。
  template <class waker_type>
    requires(!std::same_as<std::remove_cvref_t<waker_type>,
                           worker_waker_ref>) &&
                requires(waker_type &waker) {
                  { waker.wake_up() } noexcept -> std::same_as<void>;
                }
  explicit worker_waker_ref(waker_type &waker) noexcept
      : state_(std::addressof(waker)), wake_(+[](void *state) noexcept {
          static_cast<waker_type *>(state)->wake_up();
        }) {}
  // 借用对象在注册期间存活；空槽位没有唤醒动作。
  void wake_up() const noexcept {
    if (wake_)
      wake_(state_);
  }

private:
  void *state_{};                    // 借用 I/O 引擎或其他底层唤醒实现。
  void (*wake_)(void *) noexcept {}; // 与借用对象真实类型匹配的静态适配函数。
};

// 一个 runtime 一个调度域：拥有全局队列、线程协调状态及本地队列注册表。
// 不拥有 worker、I/O 引擎和根任务计数，不依赖它们的具体类型。
class domain_scheduler {
public:
  using local_queue_type =
      local_ready_queue<LOCAL_QUEUE_CAPACITY>; // 所有注册项采用相同固定容量。

  // 注册槽位启动时一次分配；运行阶段注册表不变，窃取不获取注册表锁。
  explicit domain_scheduler(std::size_t num_workers)
      : state_machine_(num_workers), registrations_(num_workers) {
    if (num_workers == 0)
      throw std::invalid_argument("调度域至少需要一个工作线程");
  }
  // 调度域身份和借用地址固定，禁止复制或迁移其注册状态。
  domain_scheduler(const domain_scheduler &) = delete;
  domain_scheduler &operator=(const domain_scheduler &) = delete;

  // 通用调度入口：线程绑定匹配本调度域时走本地队列，否则走全局队列。
  void enqueue(std::coroutine_handle<> task) {
    auto *binding = ::faio::detail::current_execution_thread;
    auto *local =
        binding
            ? binding->local_state_for<local_queue_type>(scheduler_ref{*this})
            : nullptr;
    if (local) {
      if (local->push_local(task, global_queue_))
        wake_up_one();
    } else {
      global_queue_.push_back(task);
      wake_up_one();
    }
  }
  /** @brief 主动让出与协作预算耗尽续体按 FIFO 入队，为已有等待任务保留顺序。
   * @details 普通 spawn、同步通知与 IO
   * 完成仍使用快速槽；这里只改变让出的优先顺序。
   *          所属线程按既有溢出协议发布；外部/跨域调用仍走线程安全的全局队列。
   *          入队失败以前不接管句柄，已有队列和快速槽保持原回滚合同。
   */
  void enqueue_yield(std::coroutine_handle<> task) {
    auto *binding =
        ::faio::detail::current_execution_thread; // 只借用当前稳定线程绑定。
    auto *local =
        binding
            ? binding->local_state_for<local_queue_type>(scheduler_ref{*this})
            : nullptr; // 类型与调度域都匹配才可写入线程私有发布边界。
    if (local)
      local->push_back(
          task, global_queue_); // 直接 FIFO，不驱逐或替换仍待恢复的快速任务。
    else
      global_queue_.push_back(
          task);   // 同一远程提交/关闭/分配失败语义，不访问他人私有队列。
    wake_up_one(); // FIFO 可窃取；成功发布以后仍承担一次既有同伴通知责任。
  }
  /** @brief 库内自让出续体按原 FIFO 发布，仅在有可并行工作时额外通知同伴。
   * @details 调用者发布自身句柄后立即结束 await_suspend；普通手工 enqueue_yield
   * 不提供此保证。 仅匹配 owner
   * 的本地原空/全局仍空路径省通知；续体仍可被其他搜索线程窃取。 远程提交、FIFO
   * 溢出及异常回滚保持原协议，不记录新的队列或任务状态。
   */
  void enqueue_cooperative_yield(std::coroutine_handle<> task) {
    auto *binding =
        ::faio::detail::current_execution_thread; // 只借用原稳定线程绑定。
    auto *local =
        binding
            ? binding->local_state_for<local_queue_type>(scheduler_ref{*this})
            : nullptr; // 域身份和类型都匹配才能观察私有 fast 与发布本地 FIFO。
    if (local) {
      const bool another_ready =
          !local->empty_local(); // 入队前包含 fast/FIFO，保证已有工作仍通知。
      local->push_back(
          task,
          global_queue_); // 原 FIFO/窃取/溢出/失败回滚，不改变续体优先顺序。
      // 唯一续体由正在归还执行权的 owner 领取；发布后的 global
      // 检查同时覆盖满队列转交。
      if (another_ready || !global_queue_.empty())
        wake_up_one(); // 外部并发全局提交继续沿自己的原路径承担通知责任。
    } else {
      global_queue_.push_back(
          task);     // 外部/跨域没有即将执行本域循环的 owner 保证。
      wake_up_one(); // 远程路径必须保留成功入队后的无条件通知。
    }
  }
  /** @brief driver 完成优先放入所属线程快速槽，同一 drive 末尾只通知同伴一次。
   * @details 批次最后一条保留在不可窃取的快速槽，其他条目仍进入FIFO；
   *          复用既有3次快速执行预算，减少固定IO归属上的跨线程往返。
   * @details 外部线程没有匹配绑定时回退远程入口，保持线程安全和通知责任。
   */
  void enqueue_io(std::coroutine_handle<> task) {
    auto *binding = ::faio::detail::current_execution_thread;
    auto *local =
        binding
            ? binding->local_state_for<local_queue_type>(scheduler_ref{*this})
            : nullptr;
    if (local)
      local->push_local(
          task, global_queue_); // 前一快速任务入FIFO，最后一条留在所属worker。
    else
      enqueue(task);
  }
  // 借用全局队列，供 local_scheduler 批量取任务和处理溢出。
  global_ready_queue &global_queue() noexcept { return global_queue_; }
  // 查询是否关闭，事件循环据此退出；关闭前由 shared 排空全部根帧。
  bool closed() const noexcept { return global_queue_.closed(); }
  // 关闭提交入口并通知所有底层等待者，使空闲线程也能退出。
  void close() {
    global_queue_.close();
    wake_up_all();
  }

  // 每个所属线程启动时注册自己的队列和唤醒端；所有线程注册后才开始运行。
  void register_local(std::size_t worker_id, local_queue_type &queue,
                      worker_waker_ref waker) {
    std::lock_guard lock(registrations_mutex_);
    if (worker_id >= registrations_.size() || registrations_[worker_id].queue)
      throw std::invalid_argument("本地调度器编号越界或重复注册");
    registrations_[worker_id] = worker_registration{&queue, waker};
  }
  // 全部事件循环退出后再注销；锁保证外部 close 不会访问已销毁的唤醒端。
  void unregister_local(std::size_t worker_id) noexcept {
    std::lock_guard lock(registrations_mutex_);
    registrations_[worker_id] = {};
  }
  // 从随机起点扫描，遇到可窃取任务立即停止；无需先扫描所有队列找最大值。
  std::optional<std::coroutine_handle<>>
  steal_into(std::size_t worker_id, local_queue_type &destination,
             std::size_t start) noexcept {
    const auto count = registrations_.size();
    for (std::size_t offset = 0; offset < count; ++offset) {
      const auto index = (start + offset) % count;
      if (index == worker_id)
        continue;
      // 注册表从统一启动到统一退出屏障之间保持稳定，不需要每次窃取加锁。
      auto *source = registrations_[index].queue;
      if (source && !source->empty())
        if (auto task = source->steal_into(destination))
          return task;
    }
    return std::nullopt;
  }
  // 查询固定 worker 数，用于选择随机窃取起点。
  std::size_t num_workers() const noexcept { return registrations_.size(); }

  // 领取搜索名额，限制空闲线程同时扫描队列造成的共享缓存争用。
  bool start_searching() { return state_machine_.set_searching(); }
  // 归还搜索名额；最后一个搜索者只为实际可转交的工作通知休眠同伴。
  void stop_searching() noexcept {
    if (state_machine_.cancel_searching())
      wake_up_if_work_pending(); // 私有快速槽不能被窃取，单独存在时无需唤醒其他域。
  }
  // 登记休眠并归还工作/搜索计数；最后一个搜索者必须再次检查待执行工作。
  void prepare_sleep(std::size_t worker_id, bool searching) {
    if (state_machine_.set_sleeping(worker_id, searching))
      wake_up_if_work_pending();
  }
  // 自行醒来时从休眠集合移除；false 表示已被唤醒方摘除并预先登记为搜索者。
  bool cancel_sleep(std::size_t worker_id) {
    return state_machine_.cancel_sleeping(worker_id);
  }
  // 判断底层事件返回后是否仍应等待；事件可能只是取消通知而没有就绪任务。
  bool still_sleeping(std::size_t worker_id) const {
    return state_machine_.contains(worker_id);
  }
  // 只在存在休眠线程且没有搜索者时选择一个线程，减少重复 eventfd 写入。
  void wake_up_one() noexcept {
    if (auto index = state_machine_.worker_to_notify()) {
      std::lock_guard lock(registrations_mutex_);
      registrations_[*index].waker.wake_up();
    }
  }
  // 关闭时遍历所有有效唤醒端；注销与底层唤醒的生命周期由同一把锁保护。
  void wake_up_all() noexcept {
    std::lock_guard lock(registrations_mutex_);
    for (const auto &registration : registrations_)
      registration.waker.wake_up();
  }
  // 最后一个搜索者入睡前查全局及可窃取工作，避免已有工作无人搜索。
  void wake_up_if_work_pending() noexcept {
    if (!global_queue_.empty()) {
      wake_up_one();
      return;
    }
    for (const auto &registration : registrations_)
      if (registration.queue && !registration.queue->empty()) {
        wake_up_one();
        return;
      }
  }

private:
  // 只借用调度所需的能力，调度域无需访问完整 worker 对象。
  struct worker_registration {
    local_queue_type *queue{}; // 借用本地队列；统一退出屏障前不能销毁或注销。
    worker_waker_ref waker{};  // 借用所属线程的底层唤醒能力。
  };
  global_ready_queue global_queue_;       // 跨线程提交和本地溢出的就绪队列。
  scheduler_state_machine state_machine_; // 工作、搜索、休眠线程的协调状态。
  std::vector<worker_registration>
      registrations_;              // 固定大小的本地队列/唤醒端注册表。
  std::mutex registrations_mutex_; // 只保护注册、注销和底层唤醒生命周期。
};
static_assert(coroutine_scheduler<domain_scheduler>);
} // namespace faio::runtime::detail
#endif
