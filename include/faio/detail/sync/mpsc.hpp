#ifndef FAIO_DETAIL_SYNC_MPSC_HPP
#define FAIO_DETAIL_SYNC_MPSC_HPP

#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/coroutine/coroutine_wait.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace faio::sync {
// 固定容量、多生产者单消费者队列。非阻塞收发使用每槽 sequence + CAS，
// 不拿互斥锁、不分配内存；只有满/空时才进入带互斥锁的协程等待队列。
// T 的移动必须不抛异常：一旦生产者领取槽位就必须完成发布，否则消费者会永久等待。
template <class T> class mpsc {
  static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>,
                "mpsc<T> 要求 T 可无异常移动与析构");
  struct state {
    struct cell {
      std::atomic<std::size_t> sequence{}; // 槽位所属轮次；release/acquire 发布与复用。
      std::optional<T> value; // 领取槽位者构造，唯一消费者移动并销毁。
    };
    explicit state(std::size_t capacity) : cells(capacity) {
      for (std::size_t i = 0; i < capacity; ++i)
        cells[i].sequence.store(i, std::memory_order_relaxed);
    }
    std::size_t capacity() const noexcept { return cells.size(); }
    cell& at(std::size_t pos) noexcept { return cells[pos % cells.size()]; }
    const cell& at(std::size_t pos) const noexcept { return cells[pos % cells.size()]; }

    // Vyukov 有界队列的 MPSC 快路径。CAS 只竞争写入位置；槽位的
    // release/acquire sequence 保证消费者看到完整的 T 构造。
    bool try_push(T& input) noexcept {
      // sequence 算法至少需要两个槽位；容量 1 用独立的小锁路径保持精确容量。
      if (capacity() == 1) {
        {
          std::lock_guard lock(single_mutex);
          if (single_full.load(std::memory_order_relaxed)) return false;
          single_value.emplace(std::move(input));
          single_full.store(true, std::memory_order_release);
        }
        notify_receiver();
        return true;
      }
      auto pos = tail.load(std::memory_order_relaxed);
      for (;;) {
        auto& slot = at(pos);
        const auto seq = slot.sequence.load(std::memory_order_acquire);
        const auto diff = static_cast<std::intptr_t>(seq - pos);
        if (diff == 0) {
          if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
            slot.value.emplace(std::move(input));
            slot.sequence.store(pos + 1, std::memory_order_release);
            notify_receiver();
            return true;
          }
        } else if (diff < 0) {
          return false;
        } else {
          pos = tail.load(std::memory_order_relaxed);
        }
      }
    }
    // 只能由唯一 receiver 调用；不需与其他消费者竞争 head。
    std::optional<T> try_pop() noexcept {
      if (capacity() == 1) {
        std::optional<T> result;
        {
          std::lock_guard lock(single_mutex);
          if (!single_full.load(std::memory_order_relaxed)) return std::nullopt;
          result.emplace(std::move(*single_value));
          single_value.reset();
          single_full.store(false, std::memory_order_release);
        }
        notify_sender();
        return result;
      }
      const auto pos = head.load(std::memory_order_relaxed);
      auto& slot = at(pos);
      if (slot.sequence.load(std::memory_order_acquire) != pos + 1)
        return std::nullopt;
      head.store(pos + 1, std::memory_order_relaxed);
      std::optional<T> result{std::move(*slot.value)};
      slot.value.reset();
      slot.sequence.store(pos + capacity(), std::memory_order_release);
      notify_sender();
      return result;
    }
    bool has_value() const noexcept {
      if (capacity() == 1) return single_full.load(std::memory_order_acquire);
      const auto pos = head.load(std::memory_order_relaxed);
      return at(pos).sequence.load(std::memory_order_acquire) == pos + 1;
    }
    bool has_space() const noexcept {
      if (capacity() == 1) return !single_full.load(std::memory_order_acquire);
      const auto pos = tail.load(std::memory_order_relaxed);
      return at(pos).sequence.load(std::memory_order_acquire) == pos;
    }

    void notify_receiver() {
      // 发布槽位后执行 SC 栅栏，与等待方“登记标志 → 栅栏 → 重查槽位”配对。
      // 禁止双方同时看见旧值；用栅栏代替每条消息争用共享 epoch 的 RMW。
      std::atomic_thread_fence(std::memory_order_seq_cst);
      if (!has_receivers_waiting.load(std::memory_order_seq_cst)) return;
      detail::wait_node* node;
      {
        std::lock_guard lock(wait_mutex);
        node = waiting_receivers.pop();
        has_receivers_waiting.store(waiting_receivers.head != nullptr,
                                    std::memory_order_seq_cst);
      }
      if (node) node->wake();
    }
    void notify_sender() {
      // 先释放槽位，再建立和发送等待者登记方对应的双向检查顺序。
      std::atomic_thread_fence(std::memory_order_seq_cst);
      if (!has_senders_waiting.load(std::memory_order_seq_cst)) return;
      detail::wait_node* node;
      {
        std::lock_guard lock(wait_mutex);
        node = waiting_senders.pop();
        has_senders_waiting.store(waiting_senders.head != nullptr,
                                  std::memory_order_seq_cst);
      }
      if (node) node->wake();
    }
    void drop_sender() {
      if (senders.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
      detail::wait_node* nodes;
      {
        std::lock_guard lock(wait_mutex);
        nodes = waiting_receivers.take_all();
        has_receivers_waiting.store(false, std::memory_order_seq_cst);
      }
      detail::wake_all(nodes);
    }
    void drop_receiver() {
      receiver_open.store(false, std::memory_order_release);
      detail::wait_node *send_nodes, *recv_nodes;
      {
        std::lock_guard lock(wait_mutex);
        send_nodes = waiting_senders.take_all();
        recv_nodes = waiting_receivers.take_all();
        has_senders_waiting.store(false, std::memory_order_seq_cst);
        has_receivers_waiting.store(false, std::memory_order_seq_cst);
      }
      detail::wake_all(send_nodes);
      detail::wake_all(recv_nodes);
    }

    std::vector<cell> cells; // 创建时一次性分配的固定容量环形存储。
    mutable std::mutex single_mutex; // 容量为 1 时保护单槽，不能使用序号环算法。
    std::optional<T> single_value; // 容量为 1 时实际的消息存储。
    std::atomic<bool> single_full{false}; // 单槽是否占用，等待登记检查使用 acquire。
    alignas(64) std::atomic<std::size_t> tail{0}; // 多生产者竞争的下一个写入位置。
    alignas(64) std::atomic<std::size_t> head{0}; // 唯一消费者的下一个读取位置，分离缓存行。
    std::atomic<std::size_t> senders{1}; // 发送侧控制块是否存活（1/0），不再是端点副本数量。
    std::atomic_flag receiver_busy = ATOMIC_FLAG_INIT; // 阻止多个未完成 recv 同时使用单消费者状态。
    std::atomic<bool> receiver_open{true}; // 接收端已关闭时拒绝新的发送和接收。
    std::mutex wait_mutex; // 只在背压、空等待、取消及关闭时保护等待链表。
    detail::wait_queue waiting_senders, waiting_receivers; // 借用协程帧内等待节点。
    std::atomic<bool> has_senders_waiting{false}, has_receivers_waiting{false}; // 快路径通知提示，与 SC 栅栏配对。
  };

  // 所有 sender 副本与未完成 send 共享同一个发送侧生命期控制块。
  // 最后一个引用消失时关闭发送侧；每条消息只增减 shared_ptr 引用，
  // 不再同时修改 state::senders，避免多生产者反复争用第二条计数缓存行。
  struct sender_lifetime {
    std::shared_ptr<state> shared; // 持有队列状态，接收侧可以比发送侧活得更久。
    explicit sender_lifetime(std::shared_ptr<state> value) : shared(std::move(value)) {}
    ~sender_lifetime() { shared->drop_sender(); } // 只在最后一个 sender/send 操作释放时执行。
  };
  // 此引用使用 sender_lifetime 的控制块，别名指针仍直接指向 state。
  // 因此异步 send 即使在 sender.close 后才开始，也持有发送侧开放租约。
  struct sender_lease {
    std::shared_ptr<state> shared; // 别名 shared_ptr，拥有的是发送侧控制块。
    explicit sender_lease(std::shared_ptr<state> value) : shared(std::move(value)) {}
    sender_lease(sender_lease&& other) noexcept : shared(std::move(other.shared)) {}
    sender_lease(const sender_lease&) = delete;
  };
  struct send_waiter {
    state& shared;
    detail::wait_node node;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
      {
        std::lock_guard lock(shared.wait_mutex);
        if (!shared.receiver_open.load(std::memory_order_acquire) || shared.has_space())
          return false;
        node.capture(h);
        shared.waiting_senders.push(&node);
        shared.has_senders_waiting.store(true, std::memory_order_seq_cst);
        // 和消费侧的发布栅栏配对：登记后重查，避免释放与入队交错时睡过头。
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (shared.has_space()) {
          shared.waiting_senders.remove(&node);
          shared.has_senders_waiting.store(shared.waiting_senders.head != nullptr,
                                           std::memory_order_seq_cst);
          return false;
        }
      }
      node.register_stop(shared.wait_mutex, shared.waiting_senders,
                         &shared.has_senders_waiting);
      return node.arm();
    }
    void await_resume() const {
      if (node.cancelled()) throw operation_cancelled{};
    }
  };
  struct recv_waiter {
    state& shared;
    detail::wait_node node;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
      {
        std::lock_guard lock(shared.wait_mutex);
        if (!shared.receiver_open.load(std::memory_order_acquire) ||
            shared.senders.load(std::memory_order_acquire) == 0 || shared.has_value())
          return false;
        node.capture(h);
        shared.waiting_receivers.push(&node);
        shared.has_receivers_waiting.store(true, std::memory_order_seq_cst);
        // 和生产侧的发布栅栏配对：消息已发布则撤回等待，不依赖共享 epoch。
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (shared.has_value()) {
          shared.waiting_receivers.remove(&node);
          shared.has_receivers_waiting.store(shared.waiting_receivers.head != nullptr,
                                             std::memory_order_seq_cst);
          return false;
        }
      }
      node.register_stop(shared.wait_mutex, shared.waiting_receivers,
                         &shared.has_receivers_waiting);
      return node.arm();
    }
    void await_resume() const {
      if (node.cancelled()) throw operation_cancelled{};
    }
  };

  static task<expected<void>> send_impl(sender_lease lease, T value) {
    auto& shared = *lease.shared;
    for (;;) {
      if (!shared.receiver_open.load(std::memory_order_acquire))
        co_return std::unexpected(make_error(Error::ClosedChannel));
      if (shared.try_push(value)) co_return expected<void>{};
      co_await send_waiter{shared, {}};
    }
  }
  static task<expected<T>> recv_impl(std::shared_ptr<state> shared) {
    if (!shared) co_return std::unexpected(make_error(Error::ClosedChannel));
    for (;;) {
      if (!shared->receiver_open.load(std::memory_order_acquire))
        co_return std::unexpected(make_error(Error::ClosedChannel));
      if (auto value = shared->try_pop()) co_return std::move(*value);
      if (shared->senders.load(std::memory_order_acquire) == 0)
        co_return std::unexpected(make_error(Error::ClosedChannel));
      co_await recv_waiter{*shared, {}};
    }
  }
public:
  // awaiter 本体位于调用者协程帧。无竞争路径直接尝试环形队列，
  // 只有满/空时才构造额外的 task 帧用于“醒来后再试”的循环。
  class send_operation {
  public:
    send_operation(std::shared_ptr<state> shared, T value)
        : lease_(std::move(shared)), value_(std::move(value)) {}
    send_operation(send_operation&&) noexcept = default;
    send_operation(const send_operation&) = delete;
    bool await_ready() {
      if (!lease_.shared ||
          !lease_.shared->receiver_open.load(std::memory_order_acquire)) {
        result_ = std::unexpected(make_error(Error::ClosedChannel));
        return true;
      }
      return lease_.shared->try_push(value_);
    }
    template <class Parent>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Parent> parent) {
      auto slow = send_impl(std::move(lease_), std::move(value_));
      slow_.emplace(std::move(slow).operator co_await());
      return slow_->await_suspend(parent);
    }
    expected<void> await_resume() {
      if (slow_) return slow_->await_resume();
      return std::move(result_);
    }
  private:
    sender_lease lease_;
    T value_;
    expected<void> result_{};
    std::optional<typename task<expected<void>>::awaiter> slow_;
  };

  class recv_operation {
  public:
    explicit recv_operation(std::shared_ptr<state> shared)
        : shared_(std::move(shared)), busy_(!!shared_) {
      if (busy_ && shared_->receiver_busy.test_and_set(std::memory_order_acquire))
        throw std::logic_error("mpsc 只允许一个未完成的 recv");
    }
    recv_operation(recv_operation&& other) noexcept
        : shared_(std::move(other.shared_)), busy_(std::exchange(other.busy_, false)),
          value_(std::move(other.value_)), slow_(std::move(other.slow_)) {}
    recv_operation(const recv_operation&) = delete;
    ~recv_operation() {
      if (busy_) shared_->receiver_busy.clear(std::memory_order_release);
    }
    bool await_ready() {
      if (!shared_) return true;
      if (!shared_->receiver_open.load(std::memory_order_acquire)) return true;
      if (auto item = shared_->try_pop()) value_.emplace(std::move(*item));
      return value_.has_value() ||
             shared_->senders.load(std::memory_order_acquire) == 0;
    }
    template <class Parent>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Parent> parent) {
      auto slow = recv_impl(shared_);
      slow_.emplace(std::move(slow).operator co_await());
      return slow_->await_suspend(parent);
    }
    expected<T> await_resume() {
      if (slow_) return slow_->await_resume();
      if (value_) return std::move(*value_);
      if (shared_ && !shared_->receiver_open.load(std::memory_order_acquire))
        return std::unexpected(make_error(Error::ClosedChannel));
      return std::unexpected(make_error(Error::ClosedChannel));
    }
  private:
    std::shared_ptr<state> shared_;
    bool busy_{};
    std::optional<T> value_;
    std::optional<typename task<expected<T>>::awaiter> slow_;
  };

  class sender {
  public:
    explicit sender(std::shared_ptr<state> shared) : shared_(std::move(shared)) {}
    ~sender() { close(); }
    // 所有端点共享发送侧控制块，复制不会单独修改队列状态计数。
    sender(const sender& other) = default;
    sender& operator=(const sender& other) {
      if (this != &other) {
        close();
        shared_ = other.shared_;
      }
      return *this;
    }
    sender(sender&& other) noexcept : shared_(std::move(other.shared_)) {}
    sender& operator=(sender&& other) noexcept {
      if (this != &other) { close(); shared_ = std::move(other.shared_); }
      return *this;
    }
    void close() {
      shared_.reset(); // 最后一个端点或未结束 send 才会触发 sender_lifetime 析构。
    }
    // 非协程入口先复制状态；调用者随后销毁 sender 也不会悬空 this。
    send_operation send(T value) const {
      return send_operation{shared_, std::move(value)};
    }
    // 满队列返回 false；端点关闭返回 ClosedChannel。快路径不进入等待锁。
    expected<bool> try_send(T value) const {
      if (!shared_ || !shared_->receiver_open.load(std::memory_order_acquire))
        return std::unexpected(make_error(Error::ClosedChannel));
      return shared_->try_push(value);
    }
  private:
    std::shared_ptr<state> shared_;
  };
  class receiver {
  public:
    explicit receiver(std::shared_ptr<state> shared) : shared_(std::move(shared)) {}
    ~receiver() { close(); }
    receiver(const receiver&) = delete;
    receiver& operator=(const receiver&) = delete;
    receiver(receiver&& other) noexcept : shared_(std::move(other.shared_)) {}
    receiver& operator=(receiver&& other) noexcept {
      if (this != &other) { close(); shared_ = std::move(other.shared_); }
      return *this;
    }
    void close() {
      if (shared_) { shared_->drop_receiver(); shared_.reset(); }
    }
    recv_operation recv() const { return recv_operation{shared_}; }
    expected<std::optional<T>> try_recv() const {
      if (!shared_ || !shared_->receiver_open.load(std::memory_order_acquire))
        return std::unexpected(make_error(Error::ClosedChannel));
      if (shared_->receiver_busy.test_and_set(std::memory_order_acquire))
        throw std::logic_error("mpsc 只允许一个未完成的 recv");
      struct release_busy {
        state& shared;
        ~release_busy() { shared.receiver_busy.clear(std::memory_order_release); }
      } cleanup{*shared_};
      if (auto value = shared_->try_pop()) return std::move(value);
      if (shared_->senders.load(std::memory_order_acquire) == 0)
        return std::unexpected(make_error(Error::ClosedChannel));
      return std::optional<T>{};
    }
  private:
    std::shared_ptr<state> shared_;
  };
  static std::pair<sender, receiver> make(std::size_t capacity) {
    if (capacity == 0) throw std::invalid_argument("mpsc 容量必须大于零");
    auto shared = std::make_shared<state>(capacity);
    // 每个 channel 一次发送侧控制块分配；热路径不会额外分配租约对象。
    auto lifetime = std::make_shared<sender_lifetime>(shared);
    std::shared_ptr<state> send_state{std::move(lifetime), shared.get()};
    return {sender{std::move(send_state)}, receiver{std::move(shared)}};
  }
};
} // namespace faio::sync
#endif
