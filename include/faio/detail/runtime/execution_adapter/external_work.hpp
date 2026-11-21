/**
 * @file faio/detail/runtime/execution_adapter/external_work.hpp
 * @brief 外部任务图接纳、分派与排空。
 */
#pragma once

#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/coroutine/context/thread_context.hpp"
#include "faio/detail/runtime/common/completion_service.hpp"
#include "faio/detail/coroutine/context/execution_binding.hpp"
#include "faio/detail/runtime/execution_adapter/external_work_types.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/domain_scheduler.hpp"
#include "faio/detail/runtime/common/worker_waker_ref.hpp"
#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <vector>

namespace faio::runtime::detail {
/** @brief 运行时服务与调度上下文。 */
class runtime_context;
}  // namespace faio::runtime::detail

namespace faio::detail {
/** @brief 外部执行节点的终止与清理接口。 */
template <class Derived>
class external_work_node_api {
 public:
  /** @brief 为指定目标绑定完成通知协程。 */
  template <auto Function, class Node>
  void bind(this Derived& self, Node& node) {
    self.template bind_notification<Function>(node);
  }

  /** @brief 检查是否持有可执行的通知协程。 */
  bool ready(this const Derived& self) noexcept { return static_cast<bool>(self.notification_); }

  /** @brief 先交出持有的协程帧，再执行恢复；目标可能销毁节点，此后不得再访问节点。 */
  void execute(this Derived& self) noexcept {
    auto notification = std::exchange(self.notification_, {});
    if (!notification)
      std::terminate();
    notification.resume();
  }
};

/** @brief 侵入式单次通知节点。异构目标在 bind 时生成具体通知协程，不保存手写分派回调。 */
struct external_work_node : external_work_node_api<external_work_node> {
  /** @brief 构造外部执行图的生命周期节点。 */
  external_work_node() noexcept = default;
  /** @brief 禁止复制构造外部执行图的生命周期节点。 */
  external_work_node(const external_work_node&) = delete;
  /** @brief 禁止复制赋值。 */
  external_work_node& operator=(const external_work_node&) = delete;

  /** @brief 销毁外部执行图的生命周期节点。 */
  ~external_work_node() {
    if (notification_)
      notification_.destroy();
  }

  external_work_node* next{}; /**< 侵入式任务队列后继，不拥有后继节点。 */
  external_scope_ref scope{}; /**< 外部任务作用域身份，用于传播根任务归属与取消。 */

 private:
  friend class external_work_node_api<external_work_node>;

  /** @brief 外部执行图的通知协程帧。 */
  struct notification_frame {
    /** @brief 协程返回值与挂起协议。 */
    struct promise_type {
      /** @brief 创建协程返回对象。 */
      notification_frame get_return_object() noexcept {
        return notification_frame{std::coroutine_handle<promise_type>::from_promise(*this)};
      }

      /** @brief 协程创建后保持挂起，等待显式启动。 */
      std::suspend_always initial_suspend() noexcept { return {}; }

      /** @brief 完成后直接销毁协程帧。 */
      std::suspend_never final_suspend() noexcept { return {}; }

      /** @brief 标记无返回值协程完成。 */
      void return_void() noexcept {}

      /** @brief 未捕获异常时终止进程。 */
      void unhandled_exception() noexcept { std::terminate(); }
    };

    /** @brief 构造外部执行图的通知协程帧。 */
    explicit notification_frame(std::coroutine_handle<promise_type> frame) noexcept
        : handle(frame) {}

    /** @brief 禁止复制构造外部执行图的通知协程帧。 */
    notification_frame(const notification_frame&) = delete;
    /** @brief 禁止复制赋值。 */
    notification_frame& operator=(const notification_frame&) = delete;

    std::coroutine_handle<promise_type> handle;

    /** @brief 转出持有的句柄或完成责任。 */
    std::coroutine_handle<> release() noexcept { return std::exchange(handle, {}); }

    /** @brief 销毁外部执行图的通知协程帧。 */
    ~notification_frame() {
      if (handle)
        handle.destroy();
    }
  };

  /** @brief 构造完成通知帧。 */
  template <auto Function, class Node>
  static notification_frame make_notification(external_work_node* node) {
    Function(static_cast<Node*>(node));
    co_return;
  }

  /** @brief 绑定执行图终止通知节点。 */
  template <auto Function, class Node>
  void bind_notification(Node& node) {
    static_assert(std::derived_from<Node, external_work_node>);
    static_assert(requires {
      { Function(&node) } noexcept -> std::same_as<void>;
    });
    if (std::addressof(static_cast<external_work_node&>(node)) != this)
      throw std::invalid_argument("通知帧必须绑定所属节点");
    auto next_notification = make_notification<Function, Node>(std::addressof(node));
    auto previous = std::exchange(notification_, next_notification.release());
    if (previous)
      previous.destroy();
  }

  std::coroutine_handle<> notification_{}; /**< 拥有未提交或尚未领取的通知帧。 */
};

/** @brief 逻辑退出与共享元数据的生命周期分开管理；退出后的 scheduler 副本不再借用 host。 */
struct external_root_state : std::enable_shared_from_this<external_root_state> {
  /** @brief 销毁外部执行图根状态。 */
  ~external_root_state();

  /** @brief 取得所属任务或服务的协作停止令牌。 */
  std::stop_token stop_token() const noexcept { return stop_.get_token(); }

  /** @brief 向当前任务及其受控后代传播协作停止请求。 */
  void request_stop() noexcept { stop_.request_stop(); }

  /** @brief 登记子任务的完成责任。 */
  void child_begin() noexcept;
  /** @brief 归还子任务的完成责任。 */
  void child_end() noexcept;
  /** @brief 发布执行节点终止状态。 */
  void terminal() noexcept;
  /** @brief 处理执行图排空通知。 */
  void on_drained(external_work_node& node) noexcept;

  /** @brief 判断已接受的工作是否全部退出。 */
  bool drained() const noexcept { return retired_.load(std::memory_order_acquire); }

  /** @brief 检查根任务身份是否仍有效。 */
  bool valid() const noexcept { return !drained(); }

  /** @brief 取得外部执行宿主。 */
  external_work_host& host() const noexcept { return *host_; }

 private:
  friend class external_work_host;

  /** @brief 构造外部执行图根状态。 */
  explicit external_root_state(external_work_host* host) : host_(host) {}

  external_work_host* host_{};         /**< retired_ 为 true 后不再解引用此身份地址。 */
  external_root_state* next_{};        /**< 根任务侵入式登记链表中的后继，不拥有后继根。 */
  std::size_t identity_{};             /**< 宿主为根任务分配的唯一身份编号。 */
  std::size_t pending_{};              /**< 由 host admission mutex 保护。 */
  bool terminal_{};                    /**< 标记已交付终态，析构据此区分放弃与正常结束。 */
  bool registered_{};                  /**< 当前对象是否已经登记到宿主，析构时据此成对撤销。 */
  bool drained_hook_installed_{};      /**< 是否已安装根任务排空钩子，禁止重复安装。 */
  external_work_node* drained_node_{}; /**< 排空后应交付的稳定分派节点。 */
  std::atomic<bool> retired_{false};   /**< 根任务身份是否已经退役，阻止再次登记或提交后代。 */
  std::stop_source stop_;              /**< 本操作或任务图的停止源，向已登记等待者广播协作取消。 */

  /** @brief 外部根任务停止请求转发器。 */
  struct forward_stop {
    std::stop_source* target; /**< 接收父级停止传播的目标停止源，回调期间保持有效。 */

    void operator()() const noexcept { target->request_stop(); }
  };

  std::optional<std::stop_callback<forward_stop>>
      parent_stop_; /**< 向当前根停止源传播父级停止的回调连接。 */
};

/** @brief 外部执行图的生命周期宿主。 */
class external_work_host : public runtime_completion_service {
 public:
  using root_handle = external_root_handle; /**< 共享拥有外部根任务状态的租约类型。 */
  static constexpr std::size_t automatic_target =
      std::numeric_limits<std::size_t>::max(); /**< 自动选择目标工作线程的哨兵编号。 */
  enum class phase { accepting, quiescing, draining, stopped };

  /** @brief wake 接受已选定 worker 编号；current-thread 只有编号 0。 */
  explicit external_work_host(std::size_t workers = 1)
      : runtime_completion_service(workers), queues_(workers) {}

  /** @brief 禁止复制构造外部执行图的生命周期宿主。 */
  external_work_host(const external_work_host&) = delete;
  /** @brief 禁止复制赋值。 */
  external_work_host& operator=(const external_work_host&) = delete;

  /** @brief 销毁外部执行图的生命周期宿主。 */
  ~external_work_host() {
    if (!quiescent())
      std::terminate();
  }

  /** @brief 设置宿主所属的运行时与 I/O 服务上下文。 */
  void set_context(::faio::runtime::detail::runtime_context* context) noexcept {
    context_ = context;
  }

  /** @brief 取得关联运行时上下文的借用引用，不延长运行时生命期。 */
  ::faio::runtime::detail::runtime_context* context() const noexcept { return context_; }

  /** @brief 读取宿主当前的接纳与关闭阶段。 */
  phase state() const noexcept { return phase_.load(std::memory_order_acquire); }

  /** @brief 取得外部执行根节点租约。 */
  root_handle acquire_root() {
    root_handle root{new external_root_state{this}};
    root->parent_stop_.emplace(stop_.get_token(), external_root_state::forward_stop{&root->stop_});
    {
      std::lock_guard lock(mutex_);
      if (state() != phase::accepting)
        throw std::logic_error("external host is not accepting roots");
      root->identity_ = ++next_identity_;
      root->next_ = roots_head_;
      roots_head_ = root.get();
      root->registered_ = true;
      ++roots_;
    }
    return root;
  }

  /** @brief 判断有效的根任务是否属于本宿主。 */
  bool owns(const root_handle& root) const noexcept {
    return root && std::addressof(root->host()) == this && root->valid();
  }

  /** @brief 取得指定作用域的根状态租约。 */
  root_handle lease(external_scope_ref scope) const noexcept {
    if (scope.host != this || !scope.root_state || !scope.root_state->valid())
      return {};
    return scope.root_state->weak_from_this().lock();
  }

  /** @brief 取得外部执行子任务租约。 */
  external_child_lease acquire_child(external_scope_ref scope) const noexcept;

  /** @brief 外部执行通知容量预留。 */
  class reservation {
   public:
    /** @brief 构造空的外部执行通知预留。 */
    reservation() noexcept = default;
    /** @brief 禁止复制外部执行通知预留。 */
    reservation(const reservation&) = delete;
    /** @brief 禁止复制赋值。 */
    reservation& operator=(const reservation&) = delete;

    /** @brief 移动外部执行通知预留，清空源对象。 */
    reservation(reservation&& other) noexcept
        : host_(std::exchange(other.host_, nullptr)),
          root_(std::move(other.root_)),
          top_(other.top_) {}

    /** @brief 移动赋值。 */
    reservation& operator=(reservation&& other) noexcept {
      reset();
      host_ = std::exchange(other.host_, nullptr);
      root_ = std::move(other.root_);
      top_ = other.top_;
      return *this;
    }

    /** @brief 销毁外部执行通知容量预留。 */
    ~reservation() { reset(); }

    /** @brief 开始执行后释放接纳预留。 */
    void start() noexcept { reset(); }

    /** @brief 归还宿主接纳预留并释放根状态引用；清空后重复调用不再归还计数。 */
    void reset() noexcept {
      if (auto* host = std::exchange(host_, nullptr))
        host->release_reservation(root_, top_);
      root_.reset();
    }

   private:
    friend class external_work_host;

    /** @brief 保存宿主、根状态和顶层接纳标志。 */
    reservation(external_work_host& host, root_handle root, bool top) noexcept
        : host_(&host), root_(std::move(root)), top_(top) {}

    external_work_host* host_{}; /**< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。 */
    root_handle root_;           /**< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。 */
    bool top_{};                 /**< 是否为顶层接纳租约，停机时与后代接纳采用不同规则。 */
  };

  /** @brief 在宿主状态锁内为已接纳的根任务预留外部操作计数。 */
  reservation reserve(const root_handle& root, bool top_level = false) {
    std::lock_guard lock(mutex_);
    if (!owns(root) || state() == phase::stopped || (top_level && state() != phase::accepting))
      throw std::logic_error("external operation has no accepted root");
    ++reservations_;
    ++root->pending_;
    if (top_level)
      ++top_reservations_;
    return reservation{*this, root, top_level};
  }

  /** @brief 完成结果发布者的生命周期保护。 */
  class publisher_guard {
   public:
    /** @brief 构造完成结果发布者的生命周期保护。 */
    publisher_guard() noexcept = default;

    /** @brief 构造完成结果发布者的生命周期保护。 */
    explicit publisher_guard(root_handle root) noexcept : root_(std::move(root)) {
      if (root_) {
        host_ = std::addressof(root_->host());
        host_->add_work(root_, true);
      }
    }

    /** @brief 禁止复制构造完成结果发布者的生命周期保护。 */
    publisher_guard(const publisher_guard&) = delete;
    /** @brief 禁止复制赋值。 */
    publisher_guard& operator=(const publisher_guard&) = delete;

    /** @brief 移动构造完成结果发布者的生命周期保护。 */
    publisher_guard(publisher_guard&& other) noexcept
        : host_(std::exchange(other.host_, nullptr)), root_(std::move(other.root_)) {}

    /** @brief 销毁完成结果发布者的生命周期保护。 */
    ~publisher_guard() {
      if (host_)
        host_->release_work(root_, true);
    }

   private:
    external_work_host* host_{}; /**< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。 */
    root_handle root_;           /**< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。 */
  };

  /** @brief 外部执行节点的派发状态。 */
  class dispatch {
   public:
    /** @brief 构造外部执行节点的派发状态。 */
    dispatch() noexcept = default;
    /** @brief 禁止复制构造外部执行节点的派发状态。 */
    dispatch(const dispatch&) = delete;
    /** @brief 禁止复制赋值。 */
    dispatch& operator=(const dispatch&) = delete;

    /** @brief 移动构造外部执行节点的派发状态。 */
    dispatch(dispatch&& other) noexcept
        : host_(std::exchange(other.host_, nullptr)),
          node_(other.node_),
          root_(std::move(other.root_)) {}

    /** @brief 销毁外部执行节点的派发状态。 */
    ~dispatch() {
      if (host_)
        host_->release_dispatch(root_);
    }

    /** @brief 判断是否持有有效状态。 */
    explicit operator bool() const noexcept { return node_ != nullptr; }

    /** @brief 取得执行图节点。 */
    external_work_node* node() const noexcept { return node_; }

    /** @brief 取得执行图根节点。 */
    const root_handle& root() const noexcept { return root_; }

    /** @brief 取得结构化任务作用域。 */
    external_scope_ref scope() const noexcept { return {host_, root_.get()}; }

   private:
    friend class external_work_host;

    /** @brief 构造外部执行节点的派发状态。 */
    dispatch(external_work_host& host, external_work_node* node, root_handle root) noexcept
        : host_(&host), node_(node), root_(std::move(root)) {}

    external_work_host* host_{}; /**< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。 */
    external_work_node* node_{}; /**< 仅调用前读取；guard 析构绝不访问节点。 */
    root_handle root_;           /**< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。 */
  };

  /** @brief 在结果写入后发布完成握手，只有已经挂起的等待者才重新入队。 */
  void publish(external_work_node& node, std::size_t target = automatic_target) noexcept {
    publish_with(node, target, [this] { return waker_; });
  }

  /** @brief 自定义宿主可在本次发布中借用具体唤醒服务，不在 host 中擦除或保存它。 */
  template <class W>
  void publish(external_work_node& node,
               ::faio::runtime::detail::basic_target_waker_ref<W> waker,
               std::size_t target = automatic_target) noexcept {
    publish_with(node, target, [waker] { return waker; });
  }

 private:
  /** @brief 发布携带结果的完成状态。 */
  template <class SelectWaker>
  void publish_with(external_work_node& node,
                    std::size_t target,
                    SelectWaker select_waker) noexcept {
    root_handle root;
    decltype(select_waker()) waker;
    {
      std::lock_guard lock(mutex_);
      if (!node.ready() || state() == phase::stopped)
        std::terminate();
      if (target == automatic_target) {
        const auto* binding = ::faio::detail::current_execution_binding();
        target = binding && binding->external_host() == this ? binding->worker_id()
                                                             : next_worker_++ % queues_.size();
      }
      if (target >= queues_.size())
        std::terminate();
      if (node.scope.root_state) {
        if (node.scope.host != this || !node.scope.root_state->valid())
          std::terminate();
        root = node.scope.root_state->weak_from_this().lock();
        if (!root)
          std::terminate();
        root->pending_ += 2;
      }
      node.next = nullptr;
      auto& queue = queues_[target];
      if (queue.tail)
        queue.tail->next = &node;
      else
        queue.head = &node;
      queue.tail = &node;
      queue.ready.store(true, std::memory_order_release);
      ++queued_;
      ++publishers_;
      ++wake_publishers_;
      waker = select_waker();
    }
    waker.wake_worker(target);
    {
      retirement_notification notification;
      std::lock_guard lock(mutex_);
      --publishers_;
      --wake_publishers_;
      if (root) {
        --root->pending_;
        retire_if_ready_locked(*root, notification);
      }
      signal_locked();
    }
  }

 public:
  /** @brief 在结果写入后发布完成握手，只有已经挂起的等待者才重新入队。 */
  void publish(external_work_node* node, std::size_t target = automatic_target) noexcept {
    publish(*node, target);
  }

  /** @brief 尝试领取外部提交的条目。 */
  dispatch try_pop_external(std::size_t worker) noexcept {
    std::lock_guard lock(mutex_);
    auto& queue = queues_[worker];
    auto* node = queue.head;
    if (!node)
      return {};
    queue.head = node->next;
    if (!queue.head) {
      queue.tail = nullptr;
      queue.ready.store(false, std::memory_order_release);
    }
    node->next = nullptr;
    root_handle root;
    if (node->scope.root_state) {
      root = node->scope.root_state->weak_from_this().lock();
      if (!root)
        std::terminate();
    }
    --queued_;
    ++dispatches_;
    return dispatch{*this, node, std::move(root)};
  }

  /** @brief 判断是否存在就绪任务。 */
  bool has_ready(std::size_t worker) const noexcept {
    return queues_[worker].ready.load(std::memory_order_acquire);
  }

  /** @brief 停止接纳并等待外部工作静默。 */
  void begin_quiescing() {
    std::lock_guard lock(mutex_);
    if (state() == phase::accepting) {
      if (top_reservations_)
        throw std::logic_error("external top-level operation is connected but not started");
      phase_.store(phase::quiescing, std::memory_order_release);
    }
  }

  /** @brief 开始排空已接受的工作。 */
  void begin_draining() noexcept {
    std::lock_guard lock(mutex_);
    if (state() != phase::stopped)
      phase_.store(phase::draining, std::memory_order_release);
  }

  /** @brief 向当前任务及其受控后代传播协作停止请求。 */
  void request_stop() noexcept {
    stop_.request_stop();
    notify_completion();
  }

  /** @brief 检查当前组件是否已经没有尚未归还的执行、内核引用与完成责任。 */
  bool quiescent() const noexcept {
    std::lock_guard lock(mutex_);
    return !roots_ && !reservations_ && !queued_ && !dispatches_ && !publishers_;
  }

  /** @brief 判断外部执行图是否静默。 */
  bool external_quiescent() const noexcept { return quiescent(); }

  /** @brief 通知外部线程检查完成条件。 */
  void notify_external_completion() noexcept { notify_completion(); }

  /** @brief 发布运行时停止状态。 */
  void mark_stopped() noexcept {
    std::lock_guard lock(mutex_);
    if (roots_ || reservations_ || queued_ || dispatches_ || publishers_)
      std::terminate();
    phase_.store(phase::stopped, std::memory_order_release);
    waker_ = {};
    signal_locked();
  }

 private:
  friend struct external_root_state;

  /** @brief 外部执行节点退出后的唤醒通知。 */
  struct retirement_notification {
    external_work_host* host{}; /**< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。 */
    waker_ref waker{};          /**< 具体调度域借用，唤醒发布尾部仍由 host 票据保护。 */
    std::size_t worker{};       /**< 节点应在其上执行的工作线程编号。 */

    /** @brief 销毁外部执行节点退出后的唤醒通知。 */
    ~retirement_notification() {
      if (host) {
        waker.wake_worker(worker);
        std::lock_guard lock(host->mutex_);
        --host->publishers_;
        --host->wake_publishers_;
        host->signal_locked();
      }
    }
  };

  /** @brief 外部执行节点通知队列。 */
  struct queue {
    external_work_node* head{};     /**< 最早待执行的外部节点。 */
    external_work_node* tail{};     /**< 侵入式队列末节点，追加时无需遍历；空队列为 nullptr。 */
    std::atomic<bool> ready{false}; /**< 无实验节点时 loop 只读此空提示。 */
  };

  /** @brief 在状态锁内排队执行图排空通知。 */
  void enqueue_drained_locked(external_root_state& root,
                              retirement_notification& notification) noexcept {
    auto* node = std::exchange(root.drained_node_, nullptr);
    if (!node)
      return;
    if (!node->ready() || state() == phase::stopped)
      std::terminate();
    node->scope = {};
    node->next = nullptr;
    const auto* binding = ::faio::detail::current_execution_binding();
    const auto target = binding && binding->external_host() == this
                            ? binding->worker_id()
                            : next_worker_++ % queues_.size();
    auto& queue = queues_[target];
    if (queue.tail)
      queue.tail->next = node;
    else
      queue.head = node;
    queue.tail = node;
    queue.ready.store(true, std::memory_order_release);
    ++queued_;
    ++publishers_;
    ++wake_publishers_;
    notification.host = this;
    notification.waker = waker_;
    notification.worker = target;
  }

  /** @brief 在状态锁内移除根节点登记。 */
  void remove_root_locked(external_root_state& root,
                          retirement_notification& notification) noexcept {
    auto** link = &roots_head_;
    while (*link && *link != &root)
      link = &(*link)->next_;
    if (*link != &root)
      std::terminate();
    *link = root.next_;
    root.registered_ = false;
    --roots_;
    root.retired_.store(true, std::memory_order_release);
    if (root.terminal_)
      enqueue_drained_locked(root, notification);
    if (!notification.host) {
      ++publishers_;
      ++wake_publishers_;
      notification.host = this;
      notification.waker = waker_;
      notification.worker = 0;
    }
    signal_locked();
  }

  /** @brief 完成条件满足后撤销根节点登记。 */
  void retire_if_ready_locked(external_root_state& root,
                              retirement_notification& notification) noexcept {
    if (root.registered_ && root.terminal_ && !root.pending_)
      remove_root_locked(root, notification);
  }

  /** @brief 归还宿主的操作接纳预留。 */
  void release_reservation(const root_handle& root, bool top) noexcept {
    retirement_notification notification;
    std::lock_guard lock(mutex_);
    --reservations_;
    if (top)
      --top_reservations_;
    --root->pending_;
    retire_if_ready_locked(*root, notification);
    signal_locked();
  }

  /** @brief 登记外部工作计数。 */
  void add_work(const root_handle& root, bool publisher) noexcept {
    std::lock_guard lock(mutex_);
    if (!owns(root))
      std::terminate();
    ++root->pending_;
    if (publisher)
      ++publishers_;
  }

  /** @brief 归还外部工作计数。 */
  void release_work(const root_handle& root, bool publisher) noexcept {
    retirement_notification notification;
    std::lock_guard lock(mutex_);
    --root->pending_;
    if (publisher)
      --publishers_;
    retire_if_ready_locked(*root, notification);
    signal_locked();
  }

  /** @brief 归还派发状态的引用。 */
  void release_dispatch(const root_handle& root) noexcept {
    retirement_notification notification;
    std::lock_guard lock(mutex_);
    --dispatches_;
    if (root) {
      --root->pending_;
      retire_if_ready_locked(*root, notification);
    }
    signal_locked();
  }

  /** @brief 登记根节点的子任务。 */
  void root_child(external_root_state& root, bool begin) noexcept {
    retirement_notification notification;
    std::lock_guard lock(mutex_);
    if (!root.registered_)
      std::terminate();
    if (begin)
      ++root.pending_;
    else {
      if (!root.pending_)
        std::terminate();
      --root.pending_;
      retire_if_ready_locked(root, notification);
    }
    signal_locked();
  }

  /** @brief 发布根节点终止状态。 */
  void root_terminal(external_root_state& root) noexcept {
    retirement_notification notification;
    std::lock_guard lock(mutex_);
    root.terminal_ = true;
    retire_if_ready_locked(root, notification);
    signal_locked();
  }

  /** @brief 根状态析构时检查未完成责任，并撤销根任务登记。 */
  void destroy_root(external_root_state& root) noexcept {
    retirement_notification notification;
    std::lock_guard lock(mutex_);
    if (root.pending_)
      std::terminate();
    if (root.registered_)
      remove_root_locked(root, notification);
  }

  /** @brief 安装执行图排空通知节点。 */
  void install_drained_node(external_root_state& root, external_work_node& node) noexcept {
    retirement_notification notification;
    std::lock_guard lock(mutex_);
    if (root.drained_hook_installed_)
      std::terminate();
    root.drained_hook_installed_ = true;
    root.drained_node_ = &node;
    if (root.retired_.load(std::memory_order_acquire))
      enqueue_drained_locked(root, notification);
  }

  std::vector<queue> queues_;                   /**< 每个工作线程独占消费的外部节点 FIFO 集合。 */
  external_root_state* roots_head_{};           /**< 宿主当前根任务的侵入式链表头。 */
  std::size_t next_identity_{}, next_worker_{}; /**< 自动分配外部工作目标线程时的轮转位置。 */
  std::size_t roots_{}, reservations_{}, top_reservations_{}, queued_{},
      dispatches_{};                           /**< 已领取且分派守卫尚未退出的外部节点数量。 */
  std::atomic<phase> phase_{phase::accepting}; /**< 宿主接纳、静默、排空与关闭阶段的原子状态。 */
  std::stop_source stop_; /**< 本操作或任务图的停止源，向已登记等待者广播协作取消。 */
  ::faio::runtime::detail::runtime_context*
      context_{}; /**< 借用运行时上下文，其生命期必须覆盖本操作。 */
};

/**
 * @brief 派生的原生根任务和阻塞任务可以不带 tracker；独立的子任务责任仍维持根任务的逻辑生命周期。
 * 共享元数据本身不延迟逻辑退出，最后一次 child_end 才归还责任。
 */
class external_child_lease {
 public:
  /** @brief 构造外部执行图子任务计数租约。 */
  external_child_lease() noexcept = default;

  /** @brief 构造外部执行图子任务计数租约。 */
  explicit external_child_lease(std::shared_ptr<external_root_state> root) noexcept
      : root_(std::move(root)) {
    if (root_)
      root_->child_begin();
  }

  /** @brief 禁止复制构造外部执行图子任务计数租约。 */
  external_child_lease(const external_child_lease&) = delete;
  /** @brief 禁止复制赋值。 */
  external_child_lease& operator=(const external_child_lease&) = delete;

  /** @brief 移动构造外部执行图子任务计数租约。 */
  external_child_lease(external_child_lease&& other) noexcept : root_(std::move(other.root_)) {}

  /** @brief 移动赋值。 */
  external_child_lease& operator=(external_child_lease&& other) noexcept {
    reset();
    root_ = std::move(other.root_);
    return *this;
  }

  /** @brief 销毁外部执行图子任务计数租约。 */
  ~external_child_lease() { reset(); }

  /** @brief 判断是否持有有效状态。 */
  explicit operator bool() const noexcept { return static_cast<bool>(root_); }

  /** @brief 归还一次子任务完成计数并清空根状态引用；空租约不执行任何操作。 */
  void reset() noexcept {
    if (auto root = std::move(root_))
      root->child_end();
  }

 private:
  std::shared_ptr<external_root_state>
      root_; /**< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。 */
};

/** @brief 取得外部执行子任务租约。 */
inline external_child_lease external_work_host::acquire_child(
    external_scope_ref scope) const noexcept {
  return external_child_lease{lease(scope)};
}

/** @brief 销毁外部执行图根状态。 */
inline external_root_state::~external_root_state() {
  parent_stop_.reset();
  if (!retired_.load(std::memory_order_acquire))
    host_->destroy_root(*this);
}

/** @brief 登记子任务的完成责任。 */
inline void external_root_state::child_begin() noexcept {
  host_->root_child(*this, true);
}

/** @brief 归还子任务的完成责任。 */
inline void external_root_state::child_end() noexcept {
  host_->root_child(*this, false);
}

/** @brief 发布执行节点终止状态。 */
inline void external_root_state::terminal() noexcept {
  if (!drained())
    host_->root_terminal(*this);
}

/** @brief 处理执行图排空通知。 */
inline void external_root_state::on_drained(external_work_node& node) noexcept {
  host_->install_drained_node(*this, node);
}

/** @brief 检查根任务身份是否仍有效。 */
inline bool external_scope_ref::valid() const noexcept {
  return host && root_state && std::addressof(root_state->host()) == host && root_state->valid();
}

/** @brief 判断是否持有有效状态。 */
inline external_scope_ref::operator bool() const noexcept {
  return valid();
}

using external_reservation = external_work_host::reservation;
using external_dispatch = external_work_host::dispatch;
using external_publisher_guard = external_work_host::publisher_guard;
}  // namespace faio::detail

#include "faio/detail/runtime/common/coroutine_integration.hpp"

#endif
