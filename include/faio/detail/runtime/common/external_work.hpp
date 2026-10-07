#pragma once

// 此文件无实验宏和 execution 协议依赖，C++23/C++26 TU 使用同一宿主布局。
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
  external_work_host* host{};
  external_root_state* root_state{};
  explicit operator bool() const noexcept;
  bool valid() const noexcept;
};

struct external_work_node {
  external_work_node* next{};
  void (*execute)(external_work_node*) noexcept{};
  external_scope_ref scope{};
};

// 逻辑退役和共享元数据寿命分开。退役后的 scheduler 副本不再借用 host。
struct external_root_state : std::enable_shared_from_this<external_root_state> {
  ~external_root_state();
  std::stop_token stop_token() const noexcept { return stop_.get_token(); }
  void request_stop() noexcept { stop_.request_stop(); }
  void child_begin() noexcept;
  void child_end() noexcept;
  void terminal() noexcept;
  void on_drained(external_work_node& node) noexcept;
  bool drained() const noexcept { return retired_.load(std::memory_order_acquire); }
  bool valid() const noexcept { return !drained(); }
  external_work_host& host() const noexcept { return *host_; }

 private:
  friend class external_work_host;
  explicit external_root_state(external_work_host* host) : host_(host) {}
  external_work_host* host_{};  // retired_ 为 true 后不再解引用此身份地址。
  external_root_state* next_{};
  std::size_t identity_{};
  std::size_t pending_{};  // 在 host admission mutex 下保护。
  bool terminal_{};
  bool registered_{};
  bool drained_hook_installed_{};
  external_work_node* drained_node_{};
  std::atomic<bool> retired_{false};
  std::stop_source stop_;
  struct forward_stop {
    std::stop_source* target;
    void operator()() const noexcept { target->request_stop(); }
  };
  std::optional<std::stop_callback<forward_stop>> parent_stop_;
};

class external_work_host {
 public:
  using root_handle = std::shared_ptr<external_root_state>;
  static constexpr std::size_t automatic_target = std::numeric_limits<std::size_t>::max();
  enum class phase { accepting, quiescing, draining, stopped };

  // wake 接受已选定 worker 编号；current-thread 只有编号 0。
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
  ::faio::runtime::detail::runtime_context* context() const noexcept { return context_; }
  std::size_t worker_count() const noexcept { return queues_.size(); }
  phase state() const noexcept { return phase_.load(std::memory_order_acquire); }

  root_handle acquire_root() {
    // 分配只发生于 connect/run，start/publish 路径不分配。
    root_handle root{new external_root_state{this}};
    // 回调只借用 root 自己的 source，retired metadata 可比 host 活得更久。
    // 构造/同步停止回调在准入锁外，所有 source/回调均为标准库对象。
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
    reservation() noexcept = default;
    reservation(const reservation&) = delete;
    reservation& operator=(const reservation&) = delete;
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
    void start() noexcept { reset(); }
    void reset() noexcept {
      if (auto* host = std::exchange(host_, nullptr))
        host->release_reservation(root_, top_);
      root_.reset();
    }
   private:
    friend class external_work_host;
    reservation(external_work_host& host, root_handle root, bool top) noexcept
        : host_(&host), root_(std::move(root)), top_(top) {}
    external_work_host* host_{};
    root_handle root_;
    bool top_{};
  };

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
    external_work_host* host_{};
    root_handle root_;
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
    external_work_host* host_{};
    external_work_node* node_{};  // 仅调用前读取；guard 析构绝不访问节点。
    root_handle root_;
  };

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
        root->pending_ += 2;  // queued 及锁外 waker publisher，各自保留完整责任。
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
      ++publishers_;  // 完成入队后仍保留 waker 的生命期。
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
      // queued 转 dispatch，root 的 pending 责任连续不归零。
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
  void request_stop() noexcept {
    stop_.request_stop();
    notify_completion();
  }
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
  // A kernel wake may be consumed before its publisher releases the final
  // quiescence ticket. The current-thread driver settles that bounded tail
  // before its final completion check and kernel sleep. User receiver guards
  // are deliberately excluded: waiting for arbitrary user code could stall I/O.
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
    external_work_host* host{};
    void* state{};
    void (*wake)(void*, std::size_t) noexcept{};
    std::size_t worker{};
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
    external_work_node* head{};
    external_work_node* tail{};
    std::atomic<bool> ready{false};  // 无实验节点时 loop 只读此空提示。
  };
  void enqueue_drained_locked(external_root_state& root, retirement_notification& notification) noexcept {
    auto* node = std::exchange(root.drained_node_, nullptr);
    if (!node)
      return;
    if (!node->execute || state() == phase::stopped)
      std::terminate();
    node->scope = {};  // 清理通知不借用已经退役的 root。
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
      // 无 ready node 的完成也必须唤醒 current-thread generic drive。
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

  mutable std::mutex mutex_;
  std::atomic<std::size_t> revision_{0};
  std::vector<queue> queues_;
  external_root_state* roots_head_{};
  std::size_t next_identity_{}, next_worker_{};
  std::size_t roots_{}, reservations_{}, top_reservations_{}, queued_{}, dispatches_{}, publishers_{};
  std::size_t wake_publishers_{};
  std::atomic<phase> phase_{phase::accepting};
  std::stop_source stop_;
  ::faio::runtime::detail::runtime_context* context_{};
  void* wake_state_{};
  void (*wake_)(void*, std::size_t) noexcept{};
};

// 派生 native 根和 blocking job 可以没有 tracker；独立 child 责任仍保留
// root 的逻辑寿命。共享元数据本身不延迟退役，最后 child_end 才归还责任。
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
  void reset() noexcept {
    if (auto root = std::move(root_))
      root->child_end();
  }
 private:
  std::shared_ptr<external_root_state> root_;
};

inline external_child_lease external_work_host::acquire_child(external_scope_ref scope) const noexcept {
  return external_child_lease{lease(scope)};
}

inline external_root_state::~external_root_state() {
  parent_stop_.reset();  // 等停止回调退出，不能在 host mutex 内注销。
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
inline bool external_scope_ref::valid() const noexcept {
  return host && root_state && std::addressof(root_state->host()) == host && root_state->valid();
}
inline external_scope_ref::operator bool() const noexcept { return valid(); }
using external_reservation = external_work_host::reservation;
using external_dispatch = external_work_host::dispatch;
using external_publisher_guard = external_work_host::publisher_guard;
}  // namespace faio::detail
