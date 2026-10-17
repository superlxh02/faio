/**
 * @file faio/detail/runtime/common/external_work.hpp
 * @brief 运行时调度、线程协调与定时驱动：外部任务图接纳、分派与排空。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#pragma once

/// @brief 此文件无实验宏和 execution 协议依赖，C++23/C++26 TU 使用同一宿主布局。
#include "faio/detail/coroutine/execution_thread.hpp"
#include <atomic>
#include <condition_variable>
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

namespace faio::runtime::detail { class runtime_context; }
namespace faio::detail {
class external_work_host;
struct external_root_state;
class external_child_lease;

struct external_scope_ref {
  external_work_host* host{};  ///< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。
  external_root_state* root_state{};  ///< 借用当前根任务的稳定状态，必须覆盖所关联节点的执行期。
  explicit operator bool() const noexcept;
  bool valid() const noexcept;
};

struct external_work_node {
  external_work_node* next{};  ///< 侵入式任务队列后继，不拥有后继节点。
  void (*execute)(external_work_node*) noexcept{};  ///< 节点被领取后执行的静态回调，回调可销毁节点所属对象。
  external_scope_ref scope{};  ///< 外部任务作用域身份，用于传播根任务归属与取消。
};

/// @brief 逻辑退役和共享元数据寿命分开。退役后的 scheduler 副本不再借用 host。
struct external_root_state : std::enable_shared_from_this<external_root_state> {
  ~external_root_state();
  /// @brief 取得所属任务或服务的协作停止令牌。
  std::stop_token stop_token() const noexcept { return stop_.get_token(); }
  /// @brief 向当前任务及其受控后代传播协作停止请求。
  void request_stop() noexcept { stop_.request_stop(); }
  void child_begin() noexcept;
  void child_end() noexcept;
  void terminal() noexcept;
  void on_drained(external_work_node& node) noexcept;
  bool drained() const noexcept { return retired_.load(std::memory_order_acquire); }
  /// @brief 检查当前包装是否持有有效资源身份。
  bool valid() const noexcept { return !drained(); }
  external_work_host& host() const noexcept { return *host_; }

 private:
  friend class external_work_host;
  explicit external_root_state(external_work_host* host) : host_(host) {}
  external_work_host* host_{};  ///< retired_ 为 true 后不再解引用此身份地址。
  external_root_state* next_{};  ///< 根任务侵入式登记链表中的后继，不拥有后继根。
  std::size_t identity_{};  ///< 读写半部共享的身份租约，合并时验证来自同一原始流。
  std::size_t pending_{};  ///< 在 host admission mutex 下保护。
  bool terminal_{};  ///< 标记已交付终态，析构据此区分放弃与正常结束。
  bool registered_{};  ///< 当前对象是否已经登记到宿主，析构时据此成对撤销。
  bool drained_hook_installed_{};  ///< 是否已安装根任务排空钩子，禁止重复安装。
  external_work_node* drained_node_{};  ///< 排空后应交付的稳定分派节点。
  std::atomic<bool> retired_{false};  ///< 根任务身份是否已经退役，阻止再次登记或提交后代。
  std::stop_source stop_;  ///< 本操作或任务图的停止源，向已登记等待者广播协作取消。
  struct forward_stop {
    std::stop_source* target;  ///< 接收父级停止传播的目标停止源，回调期间保持有效。
    void operator()() const noexcept { target->request_stop(); }
  };
  std::optional<std::stop_callback<forward_stop>> parent_stop_;  ///< 向当前根停止源传播父级停止的回调连接。
};

class external_work_host {
 public:
  using root_handle = std::shared_ptr<external_root_state>;  ///< 共享拥有外部根任务状态的租约类型。
  static constexpr std::size_t automatic_target = std::numeric_limits<std::size_t>::max();  ///< 自动选择目标工作线程的哨兵编号。
  enum class phase { accepting, quiescing, draining, stopped };

  /// @brief wake 接受已选定 worker 编号；current-thread 只有编号 0。
  explicit external_work_host(std::size_t workers = 1) : queues_(workers) {
    if (!workers)
      throw std::invalid_argument("external host requires a worker");
  }
  external_work_host(const external_work_host&) = delete;
  external_work_host& operator=(const external_work_host&) = delete;
  ~external_work_host() {
    if (!quiescent())
      std::terminate();
  }

  void set_waker(void* state, void (*wake)(void*, std::size_t) noexcept) noexcept {
    std::lock_guard lock(mutex_);
    wake_state_ = state;
    wake_ = wake;
  }
  void set_context(::faio::runtime::detail::runtime_context* context) noexcept { context_ = context; }
  /// @brief 取得关联运行时上下文的借用引用，不延长运行时生命期。
  ::faio::runtime::detail::runtime_context* context() const noexcept { return context_; }
  std::size_t worker_count() const noexcept { return queues_.size(); }
  phase state() const noexcept { return phase_.load(std::memory_order_acquire); }

  root_handle acquire_root() {
    /// @brief 分配只发生于 connect/run，start/publish 路径不分配。
    root_handle root{new external_root_state{this}};
    /// @brief 回调只借用 root 自己的 source，retired metadata 可比 host 活得更久。
    /// 构造/同步停止回调在准入锁外，所有 source/回调均为标准库对象。
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

  bool owns(const root_handle& root) const noexcept {
    return root && std::addressof(root->host()) == this && root->valid();
  }
  root_handle lease(external_scope_ref scope) const noexcept {
    if (scope.host != this || !scope.root_state || !scope.root_state->valid())
      return {};
    return scope.root_state->weak_from_this().lock();
  }
  external_child_lease acquire_child(external_scope_ref scope) const noexcept;

  class reservation {
   public:
    /// @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。
    reservation() noexcept = default;
    /// @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。
    reservation(const reservation&) = delete;
    reservation& operator=(const reservation&) = delete;
    /// @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。
    reservation(reservation&& other) noexcept
        : host_(std::exchange(other.host_, nullptr)), root_(std::move(other.root_)),
          top_(other.top_) {}
    reservation& operator=(reservation&& other) noexcept {
      reset();
      host_ = std::exchange(other.host_, nullptr);
      root_ = std::move(other.root_);
      top_ = other.top_;
      return *this;
    }
    ~reservation() { reset(); }
    /// @brief 把已连接的稳定操作交给其执行协议，之后只能交付一个终态。
    void start() noexcept { reset(); }
    /// @brief 按该类型约定释放或清空当前状态，重复调用遵循对象的空状态规则。
    void reset() noexcept {
      if (auto* host = std::exchange(host_, nullptr))
        host->release_reservation(root_, top_);
      root_.reset();
    }
   private:
    friend class external_work_host;
    /// @brief 设置组合操作的稳定身份，后续请求据此核对方向预留。
    reservation(external_work_host& host, root_handle root, bool top) noexcept
        : host_(&host), root_(std::move(root)), top_(top) {}
    external_work_host* host_{};  ///< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。
    root_handle root_;  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
    bool top_{};  ///< 是否为顶层接纳租约，停机时与后代接纳采用不同规则。
  };

  /// @brief 在资源状态锁内取得指定方向的组合操作预留。
  reservation reserve(const root_handle& root, bool top_level = false) {
    std::lock_guard lock(mutex_);
    if (!owns(root) || state() == phase::stopped
        || (top_level && state() != phase::accepting))
      throw std::logic_error("external operation has no accepted root");
    ++reservations_;
    ++root->pending_;
    if (top_level)
      ++top_reservations_;
    return reservation{*this, root, top_level};
  }

  class publisher_guard {
   public:
    publisher_guard() noexcept = default;
    explicit publisher_guard(root_handle root) noexcept : root_(std::move(root)) {
      if (root_) {
        host_ = std::addressof(root_->host());
        host_->add_work(root_, true);
      }
    }
    publisher_guard(const publisher_guard&) = delete;
    publisher_guard& operator=(const publisher_guard&) = delete;
    publisher_guard(publisher_guard&& other) noexcept
        : host_(std::exchange(other.host_, nullptr)), root_(std::move(other.root_)) {}
    ~publisher_guard() {
      if (host_)
        host_->release_work(root_, true);
    }
   private:
    external_work_host* host_{};  ///< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。
    root_handle root_;  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
  };

  class dispatch {
   public:
    dispatch() noexcept = default;
    dispatch(const dispatch&) = delete;
    dispatch& operator=(const dispatch&) = delete;
    dispatch(dispatch&& other) noexcept
        : host_(std::exchange(other.host_, nullptr)), node_(other.node_),
          root_(std::move(other.root_)) {}
    ~dispatch() {
      if (host_)
        host_->release_dispatch(root_);
    }
    explicit operator bool() const noexcept { return node_ != nullptr; }
    external_work_node* node() const noexcept { return node_; }
    const root_handle& root() const noexcept { return root_; }
    external_scope_ref scope() const noexcept { return {host_, root_.get()}; }
   private:
    friend class external_work_host;
    dispatch(external_work_host& host, external_work_node* node, root_handle root) noexcept
        : host_(&host), node_(node), root_(std::move(root)) {}
    external_work_host* host_{};  ///< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。
    external_work_node* node_{};  ///< 仅调用前读取；guard 析构绝不访问节点。
    root_handle root_;  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
  };

  /// @brief 在结果写入后发布完成握手，只有已经挂起的等待者才重新入队。
  void publish(external_work_node& node, std::size_t target = automatic_target) noexcept {
    root_handle root;
    void* wake_state;
    void (*wake)(void*, std::size_t) noexcept;
    {
      std::lock_guard lock(mutex_);
      if (!node.execute || state() == phase::stopped)
        std::terminate();
      if (target == automatic_target) {
        const auto* binding = current_execution_thread;
        target = binding && binding->external_host() == this
                     ? binding->worker_id()
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
        /// @brief queued 及锁外 waker publisher，各自保留完整责任。
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
      /// @brief 完成入队后仍保留 waker 的生命期。
      ++publishers_;
      ++wake_publishers_;
      wake_state = wake_state_;
      wake = wake_;
    }
    if (wake)
      wake(wake_state, target);
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
  /// @brief 在结果写入后发布完成握手，只有已经挂起的等待者才重新入队。
  void publish(external_work_node* node, std::size_t target = automatic_target) noexcept {
    publish(*node, target);
  }

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
      /// @brief queued 转 dispatch，root 的 pending 责任连续不归零。
    }
    --queued_;
    ++dispatches_;
    return dispatch{*this, node, std::move(root)};
  }
  bool has_ready(std::size_t worker) const noexcept {
    return queues_[worker].ready.load(std::memory_order_acquire);
  }

  void begin_quiescing() {
    std::lock_guard lock(mutex_);
    if (state() == phase::accepting) {
      if (top_reservations_)
        throw std::logic_error("external top-level operation is connected but not started");
      phase_.store(phase::quiescing, std::memory_order_release);
    }
  }
  void begin_draining() noexcept {
    std::lock_guard lock(mutex_);
    if (state() != phase::stopped)
      phase_.store(phase::draining, std::memory_order_release);
  }
  /// @brief 向当前任务及其受控后代传播协作停止请求。
  void request_stop() noexcept {
    stop_.request_stop();
    notify_completion();
  }
  /// @brief 检查当前组件是否已经没有尚未归还的执行、内核引用与完成责任。
  bool quiescent() const noexcept {
    std::lock_guard lock(mutex_);
    return !roots_ && !reservations_ && !queued_ && !dispatches_ && !publishers_;
  }
  bool external_quiescent() const noexcept { return quiescent(); }
  void mark_stopped() noexcept {
    std::lock_guard lock(mutex_);
    if (roots_ || reservations_ || queued_ || dispatches_ || publishers_)
      std::terminate();
    phase_.store(phase::stopped, std::memory_order_release);
    wake_ = nullptr;
    wake_state_ = nullptr;
    signal_locked();
  }
  void notify_completion() noexcept {
    void* wake_state;
    void (*wake)(void*, std::size_t) noexcept;
    {
      std::lock_guard lock(mutex_);
      signal_locked();
      wake_state = wake_state_;
      wake = wake_;
      ++publishers_;
      ++wake_publishers_;
    }
    if (wake)
      for (std::size_t i = 0; i < queues_.size(); ++i)
        wake(wake_state, i);
    {
      std::lock_guard lock(mutex_);
      --publishers_;
      --wake_publishers_;
      signal_locked();
    }
  }
  void notify_external_completion() noexcept { notify_completion(); }
  /// @brief 内核唤醒可能先于发布者释放最后一个静止凭证被消费；单线程驱动器在最终完成检查和内核等待前排空这段有界发布尾部。用户接收器守卫不参与该等待，以免任意用户代码阻塞 IO。
  bool wait_for_wake_publisher() noexcept {
    std::size_t observed;
    {
      std::lock_guard lock(mutex_);
      if (!wake_publishers_)
        return false;
      observed = revision_.load(std::memory_order_acquire);
    }
    revision_.wait(observed, std::memory_order_acquire);
    return true;
  }
  template <class Predicate>
  void wait_until(Predicate done) {
    while (!done()) {
      const auto observed = revision_.load(std::memory_order_acquire);
      if (done())
        return;
      revision_.wait(observed, std::memory_order_acquire);
    }
  }

 private:
  friend struct external_root_state;
  struct retirement_notification {
    external_work_host* host{};  ///< 借用外部任务宿主，登记、发布和排空期间由运行时保持存活。
    void* state{};  ///< 类型擦除的借用状态地址，由配套静态回调还原真实类型。
    void (*wake)(void*, std::size_t) noexcept{};  ///< 将目标工作线程从内核等待唤醒的静态适配回调。
    std::size_t worker{};  ///< 节点应在其上执行的工作线程编号。
    ~retirement_notification() {
      if (host) {
        if (wake)
          wake(state, worker);
        std::lock_guard lock(host->mutex_);
        --host->publishers_;
        --host->wake_publishers_;
        host->signal_locked();
      }
    }
  };
  void signal_locked() noexcept {
    revision_.fetch_add(1, std::memory_order_release);
    revision_.notify_all();
  }
  struct queue {
    external_work_node* head{};  ///< 下一条待读数据或节点的起点。
    external_work_node* tail{};  ///< 侵入式队列末节点，追加时无需遍历；空队列为 nullptr。
    std::atomic<bool> ready{false};  ///< 无实验节点时 loop 只读此空提示。
  };
  void enqueue_drained_locked(external_root_state& root, retirement_notification& notification) noexcept {
    auto* node = std::exchange(root.drained_node_, nullptr);
    if (!node)
      return;
    if (!node->execute || state() == phase::stopped)
      std::terminate();
    /// @brief 清理通知不借用已经退役的 root。
    node->scope = {};
    node->next = nullptr;
    const auto* binding = current_execution_thread;
    const auto target = binding && binding->external_host() == this
                            ? binding->worker_id() : next_worker_++ % queues_.size();
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
    notification.state = wake_state_;
    notification.wake = wake_;
    notification.worker = target;
  }
  void remove_root_locked(external_root_state& root, retirement_notification& notification) noexcept {
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
      /// @brief 无 ready node 的完成也必须唤醒 current-thread generic drive。
      ++publishers_;
      ++wake_publishers_;
      notification.host = this;
      notification.state = wake_state_;
      notification.wake = wake_;
      notification.worker = 0;
    }
    signal_locked();
  }
  void retire_if_ready_locked(external_root_state& root, retirement_notification& notification) noexcept {
    if (root.registered_ && root.terminal_ && !root.pending_)
      remove_root_locked(root, notification);
  }
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
  void add_work(const root_handle& root, bool publisher) noexcept {
    std::lock_guard lock(mutex_);
    if (!owns(root))
      std::terminate();
    ++root->pending_;
    if (publisher)
      ++publishers_;
  }
  void release_work(const root_handle& root, bool publisher) noexcept {
    retirement_notification notification;
    std::lock_guard lock(mutex_);
    --root->pending_;
    if (publisher)
      --publishers_;
    retire_if_ready_locked(*root, notification);
    signal_locked();
  }
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
  void root_terminal(external_root_state& root) noexcept {
    retirement_notification notification;
    std::lock_guard lock(mutex_);
    root.terminal_ = true;
    retire_if_ready_locked(root, notification);
    signal_locked();
  }
  void destroy_root(external_root_state& root) noexcept {
    retirement_notification notification;
    std::lock_guard lock(mutex_);
    if (root.pending_)
      std::terminate();
    if (root.registered_)
      remove_root_locked(root, notification);
  }
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

  mutable std::mutex mutex_;  ///< 串行保护所属对象的共享状态；调度与用户回调应遵循该对象的锁外调用协议。
  std::atomic<std::size_t> revision_{0};  ///< 宿主状态变化代数，配合原子等待避免丢失排空通知。
  std::vector<queue> queues_;  ///< 每个工作线程独占消费的外部节点 FIFO 集合。
  external_root_state* roots_head_{};  ///< 宿主当前根任务的侵入式链表头。
  std::size_t next_identity_{},  ///< 下一个根任务身份编号。
      next_worker_{};  ///< 自动分配外部工作目标线程时的轮转位置。
  std::size_t roots_{},  ///< 当前仍由宿主管理的根任务数量。
      reservations_{},  ///< 尚未转为已发布工作的接纳租约数量。
      top_reservations_{},  ///< 尚未启动的顶层接纳租约数量，静默阶段须处理它们。
      queued_{},  ///< 已发布但尚未领取的外部节点数量。
      dispatches_{},  ///< 已领取且分派守卫尚未退出的外部节点数量。
      publishers_{};  ///< 仍在锁外发布结果的线程数，域静止必须等待它们退出。
  std::size_t wake_publishers_{};  ///< 仍可能发送内核通知的发布者数量，用于排空有界唤醒尾部。
  std::atomic<phase> phase_{phase::accepting};  ///< 宿主接纳、静默、排空与关闭阶段的原子状态。
  std::stop_source stop_;  ///< 本操作或任务图的停止源，向已登记等待者广播协作取消。
  ::faio::runtime::detail::runtime_context* context_{};  ///< 借用运行时上下文，其生命期必须覆盖本操作。
  void* wake_state_{};  ///< 底层唤醒适配回调使用的借用宿主状态。
  void (*wake_)(void*, std::size_t) noexcept{};  ///< 将目标工作线程从内核等待唤醒的静态适配回调。
};

/// @brief 派生 native 根和 blocking job 可以没有 tracker；独立 child 责任仍保留
/// root 的逻辑寿命。共享元数据本身不延迟退役，最后 child_end 才归还责任。
class external_child_lease {
 public:
  external_child_lease() noexcept = default;
  explicit external_child_lease(std::shared_ptr<external_root_state> root) noexcept
      : root_(std::move(root)) {
    if (root_)
      root_->child_begin();
  }
  external_child_lease(const external_child_lease&) = delete;
  external_child_lease& operator=(const external_child_lease&) = delete;
  external_child_lease(external_child_lease&& other) noexcept : root_(std::move(other.root_)) {}
  external_child_lease& operator=(external_child_lease&& other) noexcept {
    reset();
    root_ = std::move(other.root_);
    return *this;
  }
  ~external_child_lease() { reset(); }
  explicit operator bool() const noexcept { return static_cast<bool>(root_); }
  /// @brief 按该类型约定释放或清空当前状态，重复调用遵循对象的空状态规则。
  void reset() noexcept {
    if (auto root = std::move(root_))
      root->child_end();
  }
 private:
  std::shared_ptr<external_root_state> root_;  ///< 当前受控任务图的根状态租约，维持接纳、取消与排空归属。
};

inline external_child_lease external_work_host::acquire_child(external_scope_ref scope) const noexcept {
  return external_child_lease{lease(scope)};
}

inline external_root_state::~external_root_state() {
  /// @brief 等停止回调退出，不能在 host mutex 内注销。
  parent_stop_.reset();
  if (!retired_.load(std::memory_order_acquire))
    host_->destroy_root(*this);
}
inline void external_root_state::child_begin() noexcept { host_->root_child(*this, true); }
inline void external_root_state::child_end() noexcept { host_->root_child(*this, false); }
inline void external_root_state::terminal() noexcept {
  if (!drained())
    host_->root_terminal(*this);
}
inline void external_root_state::on_drained(external_work_node& node) noexcept {
  host_->install_drained_node(*this, node);
}
/// @brief 检查当前包装是否持有有效资源身份。
inline bool external_scope_ref::valid() const noexcept {
  return host && root_state && std::addressof(root_state->host()) == host && root_state->valid();
}
inline external_scope_ref::operator bool() const noexcept { return valid(); }
using external_reservation = external_work_host::reservation;
using external_dispatch = external_work_host::dispatch;
using external_publisher_guard = external_work_host::publisher_guard;
}
