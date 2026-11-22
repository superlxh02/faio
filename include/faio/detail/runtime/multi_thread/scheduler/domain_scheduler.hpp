/**
 * @file faio/detail/runtime/multi_thread/scheduler/domain_scheduler.hpp
 * @brief 共享调度域与跨线程窃取。
 */
#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_DOMAIN_SCHEDULER_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_DOMAIN_SCHEDULER_HPP

#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/coroutine/context/execution_binding.hpp"
#include "faio/detail/coroutine/scheduler/reference.hpp"
#include "faio/detail/runtime/common/resume_target.hpp"
#include "faio/detail/runtime/common/constants.hpp"
#include "faio/detail/runtime/common/worker_waker_ref.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/ready_queue.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/mpsc_ready_queue.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/state_machine.hpp"
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace faio::runtime::detail {

/** @brief 注册项固定存放，原子入口协调远程生产者与本地注销之间的竞争。 */
template <class W>
struct alignas(128) domain_worker_registration {
  static constexpr auto closed = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
  mutable std::atomic<std::size_t> producers{closed};
  local_ready_queue<LOCAL_QUEUE_CAPACITY>* queue{};
  mpsc_ready_queue<LOCAL_QUEUE_CAPACITY>* private_queue{};
  basic_worker_waker_ref<W> waker{};
};

/**
 * @brief 一个 runtime 一个调度域：拥有全局队列、线程协调状态及本地队列注册表。
 * 不拥有 worker、I/O 引擎和根任务计数，不依赖它们的具体类型。
 */
template <class W>
class basic_domain_scheduler {
 public:
  using local_queue_type =
      local_ready_queue<LOCAL_QUEUE_CAPACITY>; /**< 所有注册项采用相同固定容量。 */
  using private_queue_type = mpsc_ready_queue<LOCAL_QUEUE_CAPACITY>;

  /** @brief 注册槽位启动时一次分配；运行阶段注册表不变，窃取不获取注册表锁。 */
  explicit basic_domain_scheduler(std::size_t num_workers = 1)
      : state_machine_(num_workers), registrations_(num_workers) {
    if (num_workers == 0)
      throw std::invalid_argument("调度域至少需要一个工作线程");
  }

  /** @brief 调度域身份和借用地址固定，禁止复制或迁移其注册状态。 */
  basic_domain_scheduler(const basic_domain_scheduler&) = delete;

  /** @brief 禁止复制赋值。 */
  basic_domain_scheduler& operator=(const basic_domain_scheduler&) = delete;

  /** @brief 通用调度入口：线程绑定匹配本调度域时走本地队列，否则走全局队列。 */
  void enqueue(std::coroutine_handle<> task) {
    auto* binding = ::faio::detail::current_execution_binding();
    auto* local = binding
                      ? binding->local_state_for<local_queue_type>(
                            basic_scheduler_ref<basic_domain_scheduler>::from_native_handle(this))
                      : nullptr;
    if (local) {
      if (local->push_local(task, global_queue_))
        wake_up_one();
    } else {
      global_queue_.push_back(task);
      wake_up_one();
    }
  }

  /**
   * @brief 将非空任务批次交给本调度域当前 worker 的本地队列。
   * @return true 表示整批接管；当前线程未绑定本域 worker 或容量不足时返回 false，
   * 不接管任何任务，调用方继续持有全部句柄的所有权。
   */
  bool try_enqueue_local_batch(std::span<const std::coroutine_handle<>> tasks) noexcept {
    auto* binding = ::faio::detail::current_execution_binding();
    const auto self = basic_scheduler_ref<basic_domain_scheduler>::from_native_handle(this);
    auto* local = binding ? binding->local_state_for<local_queue_type>(self) : nullptr;
    if (!local)
      return false;
    const auto stealable = local->try_push_local_batch(tasks);
    if (!stealable.has_value())
      return false;
    if (*stealable)
      wake_up_one();
    return true;
  }

  /** @brief 不可窃取任务进入接收 worker 的 MPSC；当前域执行线程优先选自己。 */
  void enqueue_no_stealing(std::coroutine_handle<> task) {
    auto* binding = ::faio::detail::current_execution_binding();
    const auto self = basic_scheduler_ref<basic_domain_scheduler>::from_native_handle(this);
    if (binding && binding->matches(self)) {
      enqueue_on(task, binding->worker_id());
      return;
    }
    if (closed())
      throw std::logic_error("调度队列已关闭");
    const auto start =
        private_next_.fetch_add(1, std::memory_order_relaxed) % registrations_.size();
    for (std::size_t offset = 0; offset < registrations_.size(); ++offset) {
      const auto target = (start + offset) % registrations_.size();
      registration_lease lease{registrations_[target]};
      if (!lease)
        continue;
      publish_private(task, target, lease, true);
      return;
    }
    throw std::logic_error("没有可接收不可窃取任务的 worker");
  }

  /** @brief 定向投递不进入可窃取队列；原子租约覆盖发布及远程唤醒期间的借用。 */
  void enqueue_on(std::coroutine_handle<> task, std::size_t worker_id) {
    if (worker_id >= registrations_.size())
      throw std::invalid_argument("目标 worker 编号越界");
    registration_lease lease{registrations_[worker_id]};
    if (!lease) {
      if (closed())
        throw std::logic_error("调度队列已关闭");
      throw std::invalid_argument("目标 worker 未注册或已关闭");
    }
    auto* binding = ::faio::detail::current_execution_binding();
    const auto self = basic_scheduler_ref<basic_domain_scheduler>::from_native_handle(this);
    const bool local = binding && binding->matches(self) && binding->worker_id() == worker_id;
    publish_private(task, worker_id, lease, !local);
  }

  /** @brief 在等待责任登记前预留一次恢复；满时调用方仍在执行，可处理失败。 */
  resume_reservation reserve_resume(::faio::detail::task_scheduling_policy policy) {
    if (closed())
      throw std::logic_error("调度队列已关闭");
    const auto reserve_on = [&](std::size_t worker) {
      if (worker >= registrations_.size())
        throw std::invalid_argument("目标 worker 编号越界");
      registration_lease lease{registrations_[worker]};
      if (!lease)
        return resume_reservation{};
      return resume_reservation{lease->private_queue->reserve(), worker};
    };
    if (policy.binds_core()) {
      auto reserved = reserve_on(policy.bound_worker);
      if (!reserved.slot)
        throw std::overflow_error("绑定 worker 的恢复容量已满或不可接纳");
      return reserved;
    }
    auto* binding = ::faio::detail::current_execution_binding();
    const auto self = basic_scheduler_ref<basic_domain_scheduler>::from_native_handle(this);
    const auto preferred =
        binding && binding->matches(self)
            ? binding->worker_id()
            : private_next_.fetch_add(1, std::memory_order_relaxed) % registrations_.size();
    for (std::size_t offset = 0; offset < registrations_.size(); ++offset) {
      auto reserved = reserve_on((preferred + offset) % registrations_.size());
      if (reserved.slot)
        return reserved;
    }
    throw std::overflow_error("不可窃取恢复容量已满或不可接纳");
  }

  /** @brief 已预留的恢复不再竞争容量；短租约保护当前注册身份与定向唤醒端。 */
  void enqueue_reserved(std::coroutine_handle<> task, const resume_reservation& reserved) {
    const auto worker = reserved.worker;
    const auto slot = reserved.slot;
    if (worker >= registrations_.size())
      throw std::invalid_argument("目标 worker 编号越界");
    if (closed())
      throw std::logic_error("调度队列已关闭");
    registration_lease lease{registrations_[worker]};
    if (!lease || !slot.belongs_to(*lease->private_queue))
      throw std::logic_error("恢复槽所属 worker 已注销或更换");
    auto* binding = ::faio::detail::current_execution_binding();
    const auto self = basic_scheduler_ref<basic_domain_scheduler>::from_native_handle(this);
    const bool local = binding && binding->matches(self) && binding->worker_id() == worker;
    if (!(local ? slot.publish_local(task) : slot.publish(task)))
      throw std::logic_error("恢复槽已使用或已经关闭");
    if (!local && state_machine_.notify_worker(worker))
      lease->waker.wake_up();
  }

  /** @brief 诊断查询也持有租约；消费及就绪检查由所属 local 直接访问队列。 */
  bool has_private_task(std::size_t worker_id) const noexcept {
    if (worker_id >= registrations_.size())
      return false;
    registration_lease lease{registrations_[worker_id]};
    return lease && !lease->private_queue->empty();
  }

  /**
   * @brief 将主动让出或协作预算耗尽的续体按 FIFO 入队，保持已有等待任务的顺序。
   * @details 普通 spawn、同步通知与 I/O 完成仍使用快速槽，此处只调整让出操作的优先顺序。
   *          所属线程沿用原有溢出发布协议，外部或跨域调用仍使用线程安全的全局队列。
   *          入队失败时不接管句柄，原有队列和快速槽仍遵守原来的回滚约定。
   */
  void enqueue_yield(std::coroutine_handle<> task) {
    auto* binding = ::faio::detail::current_execution_binding();
    auto* local = binding
                      ? binding->local_state_for<local_queue_type>(
                            basic_scheduler_ref<basic_domain_scheduler>::from_native_handle(this))
                      : nullptr;
    if (local)
      local->push_back(task, global_queue_);
    else
      global_queue_.push_back(task);
    wake_up_one();
  }

  /**
   * @brief 库内主动让出的续体沿用 FIFO 发布，仅在存在可并行工作时额外通知其他线程。
   * @details 调用者发布自身句柄后立即结束 await_suspend，手动调用 enqueue_yield 不提供此保证。
   *          只有 owner 匹配、本地队列原先为空且全局队列仍为空时才省略通知；其他搜索线程仍可窃取该续体。
   *          远程提交、FIFO 溢出及异常回滚沿用原有协议，不增加队列或任务状态。
   */
  void enqueue_cooperative_yield(std::coroutine_handle<> task) {
    auto* binding = ::faio::detail::current_execution_binding();
    auto* local = binding
                      ? binding->local_state_for<local_queue_type>(
                            basic_scheduler_ref<basic_domain_scheduler>::from_native_handle(this))
                      : nullptr;
    if (local) {
      const bool another_ready = !local->empty_local();
      local->push_back(task, global_queue_);
      if (another_ready || !global_queue_.empty())
        wake_up_one();
    } else {
      global_queue_.push_back(task);
      wake_up_one();
    }
  }

  /**
   *  @brief 驱动器产生的完成任务优先进入所属线程的快速槽，同一次 drive 结束时仅通知其他线程一次。
   * @details 批次最后一项保留在不可窃取的快速槽，其余项仍进入 FIFO；
   *          沿用连续快速执行 3 次的预算，减少固定 I/O 归属导致的跨线程往返。
   * @details 外部线程没有匹配绑定时回退远程入口，保持线程安全和通知责任。
   */
  void enqueue_io(std::coroutine_handle<> task) {
    auto* binding = ::faio::detail::current_execution_binding();
    auto* local = binding
                      ? binding->local_state_for<local_queue_type>(
                            basic_scheduler_ref<basic_domain_scheduler>::from_native_handle(this))
                      : nullptr;
    if (local)
      local->push_local(task, global_queue_);
    else
      enqueue(task);
  }

  /** @brief 借用全局队列，供 local_scheduler 批量取任务和处理溢出。 */
  global_ready_queue& global_queue() noexcept { return global_queue_; }

  /** @brief 查询是否关闭，事件循环据此退出；关闭前由 shared 排空全部根帧。 */
  bool closed() const noexcept { return global_queue_.closed(); }

  /** @brief 关闭接纳并等待已取得的生产者租约退出；返回后不再有在途发布。 */
  void close() {
    {
      std::lock_guard lock(registrations_mutex_);
      global_queue_.close();
      for (auto& registration : registrations_)
        close_registration(registration);
    }
    wake_up_all();
  }

  /** @brief 启动时登记所属 local 的两类队列；发布后运行期间地址保持固定。 */
  void register_local(std::size_t worker_id,
                      local_queue_type& queue,
                      private_queue_type& private_queue,
                      basic_worker_waker_ref<W> waker) {
    std::lock_guard lock(registrations_mutex_);
    if (worker_id >= registrations_.size() || registrations_[worker_id].queue)
      throw std::invalid_argument("本地调度器编号越界或重复注册");
    if (closed())
      throw std::logic_error("调度队列已关闭");
    auto& registration = registrations_[worker_id];
    registration.queue = &queue;
    registration.private_queue = &private_queue;
    registration.waker = waker;
    registration.producers.store(0, std::memory_order_release);
  }

  /** @brief 注销先关闭原子入口并排空借用，再清除队列及唤醒端指针。 */
  void unregister_local(std::size_t worker_id) noexcept {
    std::lock_guard lock(registrations_mutex_);
    auto& registration = registrations_[worker_id];
    close_registration(registration);
    registration.queue = nullptr;
    registration.private_queue = nullptr;
    registration.waker = {};
  }

  /** @brief 从随机起点扫描，遇到可窃取任务立即停止；无需先扫描所有队列找最大值。 */
  std::optional<std::coroutine_handle<>> steal_into(std::size_t worker_id,
                                                    local_queue_type& destination,
                                                    std::size_t start) noexcept {
    const auto count = registrations_.size();
    for (std::size_t offset = 0; offset < count; ++offset) {
      const auto index = (start + offset) % count;
      if (index == worker_id)
        continue;
      auto* source = registrations_[index].queue;
      if (source && !source->empty())
        if (auto task = source->steal_into(destination))
          return task;
    }
    return std::nullopt;
  }

  /** @brief 查询固定 worker 数，用于选择随机窃取起点。 */
  std::size_t num_workers() const noexcept { return registrations_.size(); }

  /** @brief 领取搜索名额，限制空闲线程同时扫描队列造成的共享缓存争用。 */
  bool start_searching() { return state_machine_.set_searching(); }

  /** @brief 归还搜索名额；最后一个搜索者只为实际可转交的工作通知休眠同伴。 */
  void stop_searching() noexcept {
    if (state_machine_.cancel_searching())
      wake_up_if_work_pending();
  }

  /** @brief 登记休眠并归还工作/搜索计数；最后一个搜索者必须再次检查待执行工作。 */
  void prepare_sleep(std::size_t worker_id, bool searching) {
    if (state_machine_.set_sleeping(worker_id, searching))
      wake_up_if_work_pending();
  }

  /** @brief 自行醒来时从休眠集合移除；false 表示已被唤醒方摘除并预先登记为搜索者。 */
  bool cancel_sleep(std::size_t worker_id) { return state_machine_.cancel_sleeping(worker_id); }

  /** @brief 判断底层事件返回后是否仍应等待；事件可能只是取消通知而没有就绪任务。 */
  bool still_sleeping(std::size_t worker_id) const { return state_machine_.contains(worker_id); }

  /** @brief 只在存在休眠线程且没有搜索者时选择一个线程，减少重复 eventfd 写入。 */
  void wake_up_one() noexcept {
    if (auto index = state_machine_.worker_to_notify()) {
      registration_lease lease{registrations_[*index]};
      if (lease)
        lease->waker.wake_up();
    }
  }

  /** @brief 关闭时遍历所有有效唤醒端；注销与底层唤醒的生命周期由同一把锁保护。 */
  void wake_up_all() noexcept {
    std::lock_guard lock(registrations_mutex_);
    for (const auto& registration : registrations_)
      registration.waker.wake_up();
  }

  /** @brief 唤醒指定工作线程。 */
  void wake_worker(std::size_t worker) noexcept {
    if (worker >= registrations_.size())
      return;
    registration_lease lease{registrations_[worker]};
    if (lease && state_machine_.notify_worker(worker))
      lease->waker.wake_up();
  }

  /** @brief 最后一个搜索者入睡前查全局及可窃取工作，避免已有工作无人搜索。 */
  void wake_up_if_work_pending() noexcept {
    if (!global_queue_.empty()) {
      wake_up_one();
      return;
    }
    for (const auto& registration : registrations_)
      if (registration.queue && !registration.queue->empty()) {
        wake_up_one();
        return;
      }
  }

 private:
  using worker_registration = domain_worker_registration<W>;
  static constexpr auto registration_closed = worker_registration::closed;
  static constexpr auto registration_users = registration_closed - 1;

  /** @brief 调度器登记的生命周期租约。 */
  class registration_lease {
   public:
    /** @brief 构造调度器登记的生命周期租约。 */
    explicit registration_lease(const worker_registration& registration) noexcept {
      auto users = registration.producers.load(std::memory_order_relaxed);
      while (!(users & registration_closed)) {
        if (registration.producers.compare_exchange_weak(
                users, users + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
          registration_ = &registration;
          break;
        }
      }
    }

    /** @brief 禁止复制构造调度器登记的生命周期租约。 */
    registration_lease(const registration_lease&) = delete;
    /** @brief 禁止复制赋值。 */
    registration_lease& operator=(const registration_lease&) = delete;
    /** @brief 禁止移动构造调度器登记的生命周期租约。 */
    registration_lease(registration_lease&&) = delete;
    /** @brief 禁止移动赋值。 */
    registration_lease& operator=(registration_lease&&) = delete;

    /** @brief 销毁调度器登记的生命周期租约。 */
    ~registration_lease() {
      if (registration_) {
        const auto previous = registration_->producers.fetch_sub(1, std::memory_order_release);
        if (previous == registration_closed + 1)
          registration_->producers.notify_all();
      }
    }

    /** @brief 判断是否持有有效状态。 */
    explicit operator bool() const noexcept { return registration_ != nullptr; }

    /** @brief 访问所引用对象的成员。 */
    const worker_registration* operator->() const noexcept { return registration_; }

   private:
    const worker_registration* registration_{};
  };

  /** @brief 关闭工作线程登记入口，并等待生产者借用归还。 */
  static void close_registration(worker_registration& registration) noexcept {
    auto users = registration.producers.fetch_or(registration_closed, std::memory_order_acq_rel)
                 | registration_closed;
    while (users & registration_users) {
      registration.producers.wait(users, std::memory_order_acquire);
      users = registration.producers.load(std::memory_order_acquire);
    }
  }

  /** @brief 发布不可窃取的就绪任务。 */
  void publish_private(std::coroutine_handle<> task,
                       std::size_t worker_id,
                       const registration_lease& lease,
                       bool remote) {
    if (closed())
      throw std::logic_error("调度队列已关闭");
    if (remote)
      lease->private_queue->push(task);
    else
      lease->private_queue->push_local(task);
    if (remote && state_machine_.notify_worker(worker_id))
      lease->waker.wake_up();
  }

  global_ready_queue global_queue_;                /**< 跨线程提交和本地溢出的就绪队列。 */
  scheduler_state_machine state_machine_;          /**< 工作、搜索、休眠线程的协调状态。 */
  std::vector<worker_registration> registrations_; /**< 固定大小的本地队列/唤醒端注册表。 */
  std::mutex registrations_mutex_;                 /**< 仅冷路径注册、注销及关闭使用。 */
  std::atomic<std::size_t> private_next_{0};       /**< 外部不可窃取提交的轮转起点。 */
};

static_assert(coroutine_scheduler<domain_scheduler>);
}  // namespace faio::runtime::detail

#include "faio/detail/runtime/common/resume_target_impl.hpp"
#endif
