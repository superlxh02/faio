/**
 * @file faio/detail/sync/mpsc.hpp
 * @brief 有界多生产者单消费者通道。
 */
#ifndef FAIO_DETAIL_SYNC_MPSC_HPP
#define FAIO_DETAIL_SYNC_MPSC_HPP

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/wait/queue.hpp"
#include "faio/detail/coroutine/task.hpp"
#include <atomic>
#include <bit>
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
/**
 * @brief 固定容量的多生产者单消费者队列。非阻塞收发通过槽位序号和 CAS 完成，
 * 不加互斥锁、不分配内存；仅在队列满或空时进入带锁的协程等待队列。
 * T 的移动必须不抛异常：生产者一旦领取槽位就必须完成发布，否则消费者会永久等待。
 */
template <class T, class S = default_scheduler>
class mpsc {
  static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>,
                "mpsc<T> 要求 T 可无异常移动与析构");

  /** @brief MPSC 通道的共享状态。 */
  struct state {
    /** @brief MPSC 消息槽位。 */
    struct cell {
      std::atomic<std::size_t> sequence{}; /**< 槽位所属轮次；release/acquire 发布与复用。 */
      std::optional<T> value;              /**< 领取槽位者构造，唯一消费者移动并销毁。 */
    };

    /** @brief 构造 MPSC 通道共享状态。 */
    explicit state(std::size_t capacity)
        : cells(capacity), index_mask(std::has_single_bit(capacity) ? capacity - 1 : 0) {
      for (std::size_t i = 0; i < capacity; ++i)
        cells[i].sequence.store(i, std::memory_order_relaxed);
    }

    /** @brief 返回通道可容纳的最大消息数。 */
    std::size_t capacity() const noexcept { return cells.size(); }

    /** @brief 访问指定存储位置。 */
    cell& at(std::size_t pos) noexcept {
      return cells[index_mask ? (pos & index_mask) : (pos % cells.size())];
    }

    /** @brief 访问指定存储位置。 */
    const cell& at(std::size_t pos) const noexcept {
      return cells[index_mask ? (pos & index_mask) : (pos % cells.size())];
    }

    /**
     * @brief Vyukov 有界队列的 MPSC 快路径。CAS 只竞争写入位置；槽位的
     * release/acquire sequence 保证消费者看到完整的 T 构造。
     */
    bool try_push(T& input) noexcept {
      if (capacity() == 1) {
        {
          std::lock_guard lock(single_mutex);
          if (single_full.load(std::memory_order_relaxed))
            return false;
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

    /** @brief 只能由唯一 receiver 调用；不需与其他消费者竞争 head。 */
    std::optional<T> try_pop() noexcept {
      if (capacity() == 1) {
        std::optional<T> result;
        {
          std::lock_guard lock(single_mutex);
          if (!single_full.load(std::memory_order_relaxed))
            return std::nullopt;
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

    /** @brief 检查队列中是否有消息。 */
    bool has_value() const noexcept {
      if (capacity() == 1)
        return single_full.load(std::memory_order_acquire);
      const auto pos = head.load(std::memory_order_relaxed);
      return at(pos).sequence.load(std::memory_order_acquire) == pos + 1;
    }

    /** @brief 判断队列是否仍有空位。 */
    bool has_space() const noexcept {
      if (capacity() == 1)
        return !single_full.load(std::memory_order_acquire);
      const auto pos = tail.load(std::memory_order_relaxed);
      return at(pos).sequence.load(std::memory_order_acquire) == pos;
    }

    /** @brief 通知通道接收等待者。 */
    void notify_receiver() {
      std::atomic_thread_fence(std::memory_order_seq_cst);
      if (!has_receivers_waiting.load(std::memory_order_seq_cst))
        return;
      detail::basic_wait_node<S>* node;
      {
        std::lock_guard lock(wait_mutex);
        node = waiting_receivers.pop();
        has_receivers_waiting.store(waiting_receivers.head != nullptr, std::memory_order_seq_cst);
      }
      if (node)
        node->wake();
    }

    /** @brief 通知通道发送等待者。 */
    void notify_sender() {
      std::atomic_thread_fence(std::memory_order_seq_cst);
      if (!has_senders_waiting.load(std::memory_order_seq_cst))
        return;
      detail::basic_wait_node<S>* node;
      {
        std::lock_guard lock(wait_mutex);
        node = waiting_senders.pop();
        has_senders_waiting.store(waiting_senders.head != nullptr, std::memory_order_seq_cst);
      }
      if (node)
        node->wake();
    }

    /** @brief 释放发送端并更新通道关闭状态。 */
    void drop_sender() {
      if (senders.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
      detail::basic_wait_node<S>* nodes;
      {
        std::lock_guard lock(wait_mutex);
        nodes = waiting_receivers.take_all();
        has_receivers_waiting.store(false, std::memory_order_seq_cst);
      }
      detail::wake_all(nodes);
    }

    /** @brief 释放接收端并通知发送等待者。 */
    void drop_receiver() {
      receiver_open.store(false, std::memory_order_release);
      detail::basic_wait_node<S>*send_nodes, *recv_nodes;
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

    std::vector<cell> cells; /**<
                              * 创建时一次性分配的固定容量环形存储。
                              * 掩码为零时，通过取模支持任意容量；容量为 1 时使用专用单槽协议，
                              * 不访问序号环。
                              */
    const std::size_t index_mask;
    mutable std::mutex single_mutex;              /**< 容量为 1 时保护单槽，不能使用序号环算法。 */
    std::optional<T> single_value;                /**< 容量为 1 时实际的消息存储。 */
    std::atomic<bool> single_full{false};         /**< 单槽是否占用，等待登记检查使用 acquire。 */
    alignas(64) std::atomic<std::size_t> tail{0}; /**< 多生产者竞争的下一个写入位置。 */
    alignas(64) std::atomic<std::size_t> head{0}; /**< 唯一消费者的下一个读取位置，分离缓存行。 */
    std::atomic<std::size_t> senders{1}; /**< 发送侧控制块的存活标志（1/0），不表示端点副本数量。 */
    std::atomic_flag receiver_busy =
        ATOMIC_FLAG_INIT;                  /**< 阻止多个未完成 recv 同时使用单消费者状态。 */
    std::atomic<bool> receiver_open{true}; /**< 接收端已关闭时拒绝新的发送和接收。 */
    std::mutex wait_mutex;                 /**< 只在背压、空等待、取消及关闭时保护等待链表。 */
    detail::basic_wait_queue<S> waiting_senders,
        waiting_receivers; /**< 因通道空而挂起的接收者 FIFO，访问受等待队列锁保护。 */
    std::atomic<bool> has_senders_waiting{false},
        has_receivers_waiting{
            false}; /**< 接收等待队列的原子非空提示，配合登记后复查防止丢失通知。 */
  };

  /**
   * @brief 所有 sender 副本与未完成的 send 共享同一个发送侧生命周期控制块。
   * 最后一个引用释放时关闭发送侧；每条消息只增减 shared_ptr 引用，
   * 无需同时修改 state::senders，避免多个生产者反复争用第二条计数缓存行。
   */
  struct sender_lifetime {
    std::shared_ptr<state> shared; /**< 持有队列状态，接收侧可以比发送侧活得更久。 */

    /** @brief 构造发送侧生命周期控制块。 */
    explicit sender_lifetime(std::shared_ptr<state> value) : shared(std::move(value)) {}

    /** @brief 只在最后一个 sender/send 操作释放时执行。 */
    ~sender_lifetime() { shared->drop_sender(); }
  };

  /**
   * @brief 此引用使用 sender_lifetime 的控制块，别名指针仍直接指向 state。
   * 因此异步 send 即使在 sender.close 后才开始，也持有发送侧开放租约。
   */
  struct sender_lease {
    std::shared_ptr<state> shared; /**< 别名 shared_ptr，拥有的是发送侧控制块。 */

    /** @brief 构造通道发送端租约。 */
    explicit sender_lease(std::shared_ptr<state> value) : shared(std::move(value)) {}

    /** @brief 移动构造通道发送端租约。 */
    sender_lease(sender_lease&& other) noexcept : shared(std::move(other.shared)) {}

    /** @brief 禁止复制构造通道发送端租约。 */
    sender_lease(const sender_lease&) = delete;
  };

  /** @brief 通道发送等待节点。 */
  struct send_waiter {
    state& shared;                   /**< 借用通道共享状态，外部所有者须保证它在使用期间存活。 */
    detail::basic_wait_node<S> node; /**< 内嵌的通道等待节点，从入队到完成交接期间不得移动。 */

    /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
    bool await_ready() const noexcept { return false; }

    /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
    template <class Promise>
      requires detail::matching_wait_scheduler<S, Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      {
        std::lock_guard lock(shared.wait_mutex);
        if (!shared.receiver_open.load(std::memory_order_acquire) || shared.has_space())
          return false;
        node.capture(h);
        shared.waiting_senders.push(&node);
        shared.has_senders_waiting.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (shared.has_space()) {
          shared.waiting_senders.remove(&node);
          shared.has_senders_waiting.store(shared.waiting_senders.head != nullptr,
                                           std::memory_order_seq_cst);
          return false;
        }
      }
      node.register_stop(shared.wait_mutex, shared.waiting_senders, &shared.has_senders_waiting);
      return node.try_suspend();
    }

    /** @brief 取得异步操作结果。 */
    void await_resume() const {
      if (node.cancelled())
        throw operation_cancelled{};
    }
  };

  /** @brief 通道接收等待节点。 */
  struct recv_waiter {
    state& shared;                   /**< 借用通道共享状态，外部所有者须保证它在使用期间存活。 */
    detail::basic_wait_node<S> node; /**< 内嵌的通道等待节点，从入队到完成交接期间不得移动。 */

    /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
    bool await_ready() const noexcept { return false; }

    /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
    template <class Promise>
      requires detail::matching_wait_scheduler<S, Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
      {
        std::lock_guard lock(shared.wait_mutex);
        if (!shared.receiver_open.load(std::memory_order_acquire)
            || shared.senders.load(std::memory_order_acquire) == 0 || shared.has_value())
          return false;
        node.capture(h);
        shared.waiting_receivers.push(&node);
        shared.has_receivers_waiting.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (shared.has_value()) {
          shared.waiting_receivers.remove(&node);
          shared.has_receivers_waiting.store(shared.waiting_receivers.head != nullptr,
                                             std::memory_order_seq_cst);
          return false;
        }
      }
      node.register_stop(
          shared.wait_mutex, shared.waiting_receivers, &shared.has_receivers_waiting);
      return node.try_suspend();
    }

    /** @brief 取得异步操作结果。 */
    void await_resume() const {
      if (node.cancelled())
        throw operation_cancelled{};
    }
  };

  /** @brief 发送通道消息并处理等待登记。 */
  static task<expected<void>, S> send_impl(sender_lease lease, T value) {
    auto& shared = *lease.shared;
    for (;;) {
      if (!shared.receiver_open.load(std::memory_order_acquire))
        co_return std::unexpected(make_error(Error::ClosedChannel));
      if (shared.try_push(value))
        co_return expected<void>{};
      co_await send_waiter{shared, {}};
    }
  }

  /** @brief 接收通道消息并处理等待登记。 */
  static task<expected<T>, S> recv_impl(std::shared_ptr<state> shared) {
    if (!shared)
      co_return std::unexpected(make_error(Error::ClosedChannel));
    for (;;) {
      if (!shared->receiver_open.load(std::memory_order_acquire))
        co_return std::unexpected(make_error(Error::ClosedChannel));
      if (auto value = shared->try_pop())
        co_return std::move(*value);
      if (shared->senders.load(std::memory_order_acquire) == 0)
        co_return std::unexpected(make_error(Error::ClosedChannel));
      co_await recv_waiter{*shared, {}};
    }
  }

 public:
  /**
   * @brief awaiter 本体位于调用者协程帧。无竞争路径直接尝试环形队列，
   * 只有满/空时才构造额外的 task 帧用于“醒来后再试”的循环。
   */
  class send_operation {
   public:
    /** @brief 构造通道发送操作。 */
    send_operation(std::shared_ptr<state> shared, T value)
        : lease_(std::move(shared)), value_(std::move(value)) {}

    /** @brief 移动构造通道发送操作。 */
    send_operation(send_operation&&) noexcept = default;

    /** @brief 禁止复制构造通道发送操作。 */
    send_operation(const send_operation&) = delete;

    /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
    bool await_ready() {
      if (!lease_.shared || !lease_.shared->receiver_open.load(std::memory_order_acquire)) {
        result_ = std::unexpected(make_error(Error::ClosedChannel));
        return true;
      }
      return lease_.shared->try_push(value_);
    }

    /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
    template <class Parent>
      requires detail::matching_wait_scheduler<S, Parent>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Parent> parent) {
      auto slow = send_impl(std::move(lease_), std::move(value_));
      slow_.emplace(std::move(slow).operator co_await());
      return slow_->await_suspend(parent);
    }

    /** @brief 取得异步操作结果。 */
    expected<void> await_resume() {
      if (slow_)
        return slow_->await_resume();
      return std::move(result_);
    }

   private:
    sender_lease lease_;      /**< 发送端共享租约，控制通道何时不再可能产生新消息。 */
    T value_;                 /**< 待发送的消息值。 */
    expected<void> result_{}; /**< 本次操作的完成结果，包含成功状态或错误信息。 */
    std::optional<typename task<expected<void>, S>::awaiter>
        slow_; /**< 仅在慢路径创建的子协程等待器，快速成功时不分配子协程帧。 */
  };

  /** @brief 通道接收操作。 */
  class recv_operation {
   public:
    /** @brief 构造通道接收操作。 */
    explicit recv_operation(std::shared_ptr<state> shared)
        : shared_(std::move(shared)), busy_(!!shared_) {
      if (busy_ && shared_->receiver_busy.test_and_set(std::memory_order_acquire))
        throw std::logic_error("mpsc 只允许一个未完成的 recv");
    }

    /** @brief 移动构造通道接收操作。 */
    recv_operation(recv_operation&& other) noexcept
        : shared_(std::move(other.shared_)),
          busy_(std::exchange(other.busy_, false)),
          value_(std::move(other.value_)),
          slow_(std::move(other.slow_)) {}

    /** @brief 禁止复制构造通道接收操作。 */
    recv_operation(const recv_operation&) = delete;

    /** @brief 销毁通道接收操作。 */
    ~recv_operation() {
      if (busy_)
        shared_->receiver_busy.clear(std::memory_order_release);
    }

    /** @brief 检查等待条件是否已满足；返回 true 时，编译器跳过挂起登记。 */
    bool await_ready() {
      if (!shared_)
        return true;
      if (!shared_->receiver_open.load(std::memory_order_acquire))
        return true;
      if (auto item = shared_->try_pop())
        value_.emplace(std::move(*item));
      return value_.has_value() || shared_->senders.load(std::memory_order_acquire) == 0;
    }

    /** @brief 登记恢复目标，并按等待协议交接挂起和完成责任。 */
    template <class Parent>
      requires detail::matching_wait_scheduler<S, Parent>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Parent> parent) {
      auto slow = recv_impl(shared_);
      slow_.emplace(std::move(slow).operator co_await());
      return slow_->await_suspend(parent);
    }

    /** @brief 取得异步操作结果。 */
    expected<T> await_resume() {
      if (slow_)
        return slow_->await_resume();
      if (value_)
        return std::move(*value_);
      if (shared_ && !shared_->receiver_open.load(std::memory_order_acquire))
        return std::unexpected(make_error(Error::ClosedChannel));
      return std::unexpected(make_error(Error::ClosedChannel));
    }

   private:
    std::shared_ptr<state>
        shared_;             /**< 共享持有通道状态，保证它在异步执行和领取结果期间地址不变。 */
    bool busy_{};            /**< 标记本接收操作是否持有接收忙标记。 */
    std::optional<T> value_; /**< 接收到的消息值，读取前须确认接收操作已完成。 */
    std::optional<typename task<expected<T>, S>::awaiter>
        slow_; /**< 仅在慢路径创建的子协程等待器，快速成功时不分配子协程帧。 */
  };

  /** @brief 可复制的通道发送端。 */
  class sender {
   public:
    /** @brief 构造可复制的通道发送端。 */
    explicit sender(std::shared_ptr<state> shared) : shared_(std::move(shared)) {}

    /** @brief 销毁可复制的通道发送端。 */
    ~sender() { close(); }

    /** @brief 所有端点共享发送侧控制块，复制不会单独修改队列状态计数。 */
    sender(const sender& other) = default;

    /** @brief 复制赋值。 */
    sender& operator=(const sender& other) {
      if (this != &other) {
        close();
        shared_ = other.shared_;
      }
      return *this;
    }

    /** @brief 移动构造可复制的通道发送端。 */
    sender(sender&& other) noexcept : shared_(std::move(other.shared_)) {}

    /** @brief 移动赋值。 */
    sender& operator=(sender&& other) noexcept {
      if (this != &other) {
        close();
        shared_ = std::move(other.shared_);
      }
      return *this;
    }

    /** @brief 关闭当前发送端，已发起操作持有的发送侧租约继续有效。 */
    void close() { shared_.reset(); }

    /** @brief 非协程入口先复制状态；调用者随后销毁 sender 也不会悬空 this。 */
    send_operation send(T value) const { return send_operation{shared_, std::move(value)}; }

    /** @brief 满队列返回 false；端点关闭返回 ClosedChannel。快路径不进入等待锁。 */
    expected<bool> try_send(T value) const {
      if (!shared_ || !shared_->receiver_open.load(std::memory_order_acquire))
        return std::unexpected(make_error(Error::ClosedChannel));
      return shared_->try_push(value);
    }

   private:
    std::shared_ptr<state>
        shared_; /**< 共享持有通道状态，保证它在异步执行和领取结果期间地址不变。 */
  };

  /** @brief 只可移动、独占所有权的通道接收端。 */
  class receiver {
   public:
    /** @brief 构造独占的通道接收端。 */
    explicit receiver(std::shared_ptr<state> shared) : shared_(std::move(shared)) {}

    /** @brief 析构时关闭通道接收端。 */
    ~receiver() { close(); }

    /** @brief 禁止复制接收端所有权。 */
    receiver(const receiver&) = delete;

    /** @brief 禁止复制赋值。 */
    receiver& operator=(const receiver&) = delete;

    /** @brief 转移接收端的独占所有权。 */
    receiver(receiver&& other) noexcept : shared_(std::move(other.shared_)) {}

    /** @brief 移动赋值。 */
    receiver& operator=(receiver&& other) noexcept {
      if (this != &other) {
        close();
        shared_ = std::move(other.shared_);
      }
      return *this;
    }

    /** @brief 关闭接收端并通知等待者，然后释放共享状态。 */
    void close() {
      if (shared_) {
        shared_->drop_receiver();
        shared_.reset();
      }
    }

    /** @brief 发起一次通道接收，返回接收操作的结果。 */
    recv_operation recv() const { return recv_operation{shared_}; }

    /** @brief 尝试接收一条通道消息。 */
    expected<std::optional<T>> try_recv() const {
      if (!shared_ || !shared_->receiver_open.load(std::memory_order_acquire))
        return std::unexpected(make_error(Error::ClosedChannel));
      if (shared_->receiver_busy.test_and_set(std::memory_order_acquire))
        throw std::logic_error("mpsc 只允许一个未完成的 recv");

      struct release_busy {
        state& shared; /**< 借用通道共享状态，外部所有者须保证它在使用期间存活。 */

        /** @brief 清除接收忙标记。 */
        ~release_busy() { shared.receiver_busy.clear(std::memory_order_release); }
      } cleanup{*shared_};

      if (auto value = shared_->try_pop())
        return std::move(value);
      if (shared_->senders.load(std::memory_order_acquire) == 0)
        return std::unexpected(make_error(Error::ClosedChannel));
      return std::optional<T>{};
    }

   private:
    std::shared_ptr<state>
        shared_; /**< 共享持有通道状态，保证它在异步执行和领取结果期间地址不变。 */
  };

  /** @brief 创建共享同一通道的发送端和接收端。 */
  static std::pair<sender, receiver> make(std::size_t capacity) {
    if (capacity == 0)
      throw std::invalid_argument("mpsc 容量必须大于零");
    auto shared = std::make_shared<state>(capacity);
    auto lifetime = std::make_shared<sender_lifetime>(shared);
    std::shared_ptr<state> send_state{std::move(lifetime), shared.get()};
    return {sender{std::move(send_state)}, receiver{std::move(shared)}};
  }
};
}  // namespace faio::sync
#endif
