/**
 * @file faio/detail/sync/mpsc.hpp
 * @brief 协程同步原语：有界多生产者单消费者通道。
 * @details 同步等待保存帧内节点，队列只借用地址。登记、通知与取消争夺唯一恢复权；跨线程通知经保存的调度器恢复，用户协程不在工作线程上阻塞等待。
 */
#ifndef FAIO_DETAIL_SYNC_MPSC_HPP
#define FAIO_DETAIL_SYNC_MPSC_HPP

#include "faio/detail/common/error.hpp"
#include "faio/detail/coroutine/coroutine_wait.hpp"
#include "faio/detail/coroutine/task.hpp"
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
/// @brief 固定容量、多生产者单消费者队列。非阻塞收发使用每槽 sequence + CAS，
/// 不拿互斥锁、不分配内存；只有满/空时才进入带互斥锁的协程等待队列。 T
/// 的移动必须不抛异常：一旦生产者领取槽位就必须完成发布，否则消费者会永久等待。
template <class T>
class mpsc {
  static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>,
                "mpsc<T> 要求 T 可无异常移动与析构");

  struct state {
    struct cell {
      std::atomic<std::size_t> sequence{};  ///< 槽位所属轮次；release/acquire 发布与复用。
      std::optional<T> value;  ///< 领取槽位者构造，唯一消费者移动并销毁。
    };

    explicit state(std::size_t capacity) : cells(capacity) {
      /// @brief 初始化每槽所属轮次，首轮生产者可在对应位置构造消息。
      for (std::size_t i = 0; i < capacity; ++i)
        /// @brief 构造阶段尚未跨线程发布，直接设置槽序号即可，无需同步读者。
        cells[i].sequence.store(i, std::memory_order_relaxed);
    }

    /// @brief 返回当前对象允许存放的最大元素或字节数量。
    std::size_t capacity() const noexcept { return cells.size(); }

    cell& at(std::size_t pos) noexcept { return cells[pos % cells.size()]; }

    const cell& at(std::size_t pos) const noexcept { return cells[pos % cells.size()]; }

    /// @brief Vyukov 有界队列的 MPSC 快路径。CAS 只竞争写入位置；槽位的
    /// release/acquire sequence 保证消费者看到完整的 T 构造。
    bool try_push(T& input) noexcept {
      /// @brief sequence 算法至少需要两个槽位；容量 1 用独立的小锁路径保持精确容量。
      if (capacity() == 1) {
        {
          /// @brief 单槽特例在短临界区内交接消息，普通多槽收发不获取此锁。
          std::lock_guard lock(single_mutex);
          /// @brief 在单槽锁内发现已有消息时拒绝当前写入，输入值仍归发送者。
          if (single_full.load(std::memory_order_relaxed))
            return false;
          /// @brief 成功取得单槽后原位移动消息；T 的无异常移动保证已领取位置必定完成发布。
          single_value.emplace(std::move(input));
          /// @brief release 发布完整消息，等待登记方 acquire 观察后才能读取该值。
          single_full.store(true, std::memory_order_release);
        }
        /// @brief 消息发布后再通知接收等待者，唤醒方仍须与登记后复查协议配对。
        notify_receiver();
        return true;
      }
      /// @brief 读取候选生产位置，真正独占领取仍由尾索引 CAS 仲裁。
      auto pos = tail.load(std::memory_order_relaxed);
      for (;;) {
        /// @brief 按候选单调位置定位环形槽，随后核对槽序号是否属于本轮生产。
        auto& slot = at(pos);
        /// @brief acquire 观察消费者释放槽位的序号，确保上一轮对象已经完成析构。
        const auto seq = slot.sequence.load(std::memory_order_acquire);
        /// @brief 序号差为零表示可写，负数表示尚未回收，正数表示候选生产位置已经过期。
        const auto diff = static_cast<std::intptr_t>(seq - pos);
        /// @brief 该槽已回收到本轮生产者可写状态，开始竞争唯一生产索引。
        if (diff == 0) {
          /// @brief 尾索引 CAS 只仲裁槽位所有权；对象内容的跨线程可见性由槽 sequence 发布。
          if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
            /// @brief 唯一领取者在槽内构造消息，构造完成前消费者仍不能观察到本轮可读序号。
            slot.value.emplace(std::move(input));
            /// @brief 把槽发布为本轮可读状态，acquire 消费者随后可读取完整消息。
            slot.sequence.store(pos + 1, std::memory_order_release);
            /// @brief 消息发布后再通知接收等待者，唤醒方仍须与登记后复查协议配对。
            notify_receiver();
            return true;
          }
        } else if (diff < 0) {
          return false;
        } else {
          /// @brief 其他生产者已推进尾部时重新选择候选位置，不等待旧槽回收。
          pos = tail.load(std::memory_order_relaxed);
        }
      }
    }

    /// @brief 只能由唯一 receiver 调用；不需与其他消费者竞争 head。
    std::optional<T> try_pop() noexcept {
      /// @brief 单槽队列无法区分序号环的满与空，使用独立的小锁路径维持精确容量。
      if (capacity() == 1) {
        /// @brief 在消费者局部保存取出的消息，释放槽位后仍由该对象拥有返回值。
        std::optional<T> result;
        {
          /// @brief 单槽特例在短临界区内交接消息，普通多槽收发不获取此锁。
          std::lock_guard lock(single_mutex);
          /// @brief 单槽锁内没有消息时立即报告当前为空。
          if (!single_full.load(std::memory_order_relaxed))
            return std::nullopt;
          /// @brief 将单槽消息移入局部结果，随后可以销毁槽内旧值。
          result.emplace(std::move(*single_value));
          /// @brief 销毁已经移出的单槽对象，为下一条消息归还存储。
          single_value.reset();
          /// @brief 发布单槽重新可写，发送等待者在 acquire 复查后可继续。
          single_full.store(false, std::memory_order_release);
        }
        /// @brief 槽位回收后通知一个因满载而挂起的发送者。
        notify_sender();
        /// @brief 返回已从通道或结果槽独占移动出的值，原存储不再拥有该值。
        return result;
      }
      /// @brief 只有一个消费者读取并推进 head，因此索引本身无需与其他消费者竞争。
      const auto pos = head.load(std::memory_order_relaxed);
      /// @brief 按候选单调位置定位环形槽，随后核对槽序号是否属于本轮生产。
      auto& slot = at(pos);
      /// @brief 尚未观察到本轮完整消息发布时返回暂空，不读取正在构造的对象。
      if (slot.sequence.load(std::memory_order_acquire) != pos + 1)
        return std::nullopt;
      /// @brief 唯一消费者推进下一读取位置，槽内容交接仍由 sequence 保证。
      head.store(pos + 1, std::memory_order_relaxed);
      /// @brief 在归还槽位之前把消息所有权移入局部结果，生产者随后可复用原存储。
      std::optional<T> result{std::move(*slot.value)};
      /// @brief 先销毁槽内旧消息，再向生产者发布该槽可复用。
      slot.value.reset();
      /// @brief 将槽序号推进一个完整容量，向下一轮生产者发布复用资格。
      slot.sequence.store(pos + capacity(), std::memory_order_release);
      /// @brief 槽位回收后通知一个因满载而挂起的发送者。
      notify_sender();
      /// @brief 返回已从通道或结果槽独占移动出的值，原存储不再拥有该值。
      return result;
    }

    bool has_value() const noexcept {
      if (capacity() == 1)
        return single_full.load(std::memory_order_acquire);
      /// @brief 只有一个消费者读取并推进 head，因此索引本身无需与其他消费者竞争。
      const auto pos = head.load(std::memory_order_relaxed);
      return at(pos).sequence.load(std::memory_order_acquire) == pos + 1;
    }

    bool has_space() const noexcept {
      if (capacity() == 1)
        return !single_full.load(std::memory_order_acquire);
      const auto pos = tail.load(std::memory_order_relaxed);
      return at(pos).sequence.load(std::memory_order_acquire) == pos;
    }

    void notify_receiver() {
      /// @brief 发布槽位后执行 SC 栅栏，与等待方“登记标志 → 栅栏 → 重查槽位”配对。
      /// 禁止双方同时看见旧值；用栅栏代替每条消息争用共享 epoch 的 RMW。
      std::atomic_thread_fence(std::memory_order_seq_cst);
      /// @brief 全序提示证明没有登记接收等待者时跳过等待锁，登记方仍会在公布提示后复查消息。
      if (!has_receivers_waiting.load(std::memory_order_seq_cst))
        return;
      /// @brief 暂存锁内独占摘下的等待节点，释放队列锁后才向它发布恢复通知。
      detail::wait_node* node;
      {
        /// @brief 只有等待、取消与关闭路径取得此锁，普通多槽收发不串行争抢等待队列。
        std::lock_guard lock(wait_mutex);
        /// @brief 在锁内摘除最早接收等待者，当前通知方取得其唯一恢复责任。
        node = waiting_receivers.pop();
        /// @brief 摘除节点后在同一锁内修正接收等待提示，保持其与真实链表及双向复查顺序一致。
        has_receivers_waiting.store(waiting_receivers.head != nullptr, std::memory_order_seq_cst);
      }
      /// @brief 只有成功取得节点摘除权的路径才可在锁外通知该节点。
      if (node)
        /// @brief 锁外发布通知；节点可能随恢复立即销毁，通知之后不能继续访问它。
        node->wake();
    }

    void notify_sender() {
      /// @brief 先释放槽位，再建立和发送等待者登记方对应的双向检查顺序。
      std::atomic_thread_fence(std::memory_order_seq_cst);
      /// @brief 没有登记发送等待者时免去等待锁；新登记方通过栅栏后的槽位复查弥补竞争窗口。
      if (!has_senders_waiting.load(std::memory_order_seq_cst))
        return;
      /// @brief 暂存锁内独占摘下的等待节点，释放队列锁后才向它发布恢复通知。
      detail::wait_node* node;
      {
        /// @brief 只有等待、取消与关闭路径取得此锁，普通多槽收发不串行争抢等待队列。
        std::lock_guard lock(wait_mutex);
        /// @brief 在锁内摘除最早发送等待者，按 FIFO 交接一次可继续尝试的机会。
        node = waiting_senders.pop();
        /// @brief 摘除节点后同步修正发送等待提示，避免无等待者时继续走通知慢路径。
        has_senders_waiting.store(waiting_senders.head != nullptr, std::memory_order_seq_cst);
      }
      /// @brief 只有成功取得节点摘除权的路径才可在锁外通知该节点。
      if (node)
        /// @brief 锁外发布通知；节点可能随恢复立即销毁，通知之后不能继续访问它。
        node->wake();
    }

    void drop_sender() {
      /// @brief 只有最后一个发送侧生命期引用解除时才关闭发送能力并唤醒全部接收等待者。
      if (senders.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
      /// @brief 先在锁内取得整条等待链，再在锁外发布恢复，避免用户代码争用内部队列锁。
      detail::wait_node* nodes;
      {
        /// @brief 只有等待、取消与关闭路径取得此锁，普通多槽收发不串行争抢等待队列。
        std::lock_guard lock(wait_mutex);
        /// @brief 最后发送端退出后摘除全部接收等待者，使其能排空剩余消息并观察通道结束。
        nodes = waiting_receivers.take_all();
        /// @brief 整条接收等待链已移出，撤销提示，后续等待会重新检查发送端或接收端关闭状态。
        has_receivers_waiting.store(false, std::memory_order_seq_cst);
      }
      /// @brief 在队列锁外批量恢复独占摘下的等待链，唤醒前逐个保存后继。
      detail::wake_all(nodes);
    }

    void drop_receiver() {
      /// @brief 先发布接收端关闭，新的发送与等待复查随后能观察到关闭原因。
      receiver_open.store(false, std::memory_order_release);
      /// @brief 分别保存关闭时摘出的两类等待链，不能在恢复一类节点后继续借用其原链表。
      detail::wait_node *send_nodes, *recv_nodes;
      {
        /// @brief 只有等待、取消与关闭路径取得此锁，普通多槽收发不串行争抢等待队列。
        std::lock_guard lock(wait_mutex);
        /// @brief 关闭接收端时接管全部发送等待者，令其恢复并报告无法继续发送。
        send_nodes = waiting_senders.take_all();
        /// @brief 同时接管接收等待者，避免接收端关闭后留下永久挂起节点。
        recv_nodes = waiting_receivers.take_all();
        /// @brief 关闭路径已接管全部发送等待者，撤销发送等待提示以防重复通知旧链。
        has_senders_waiting.store(false, std::memory_order_seq_cst);
        /// @brief 整条接收等待链已移出，撤销提示，后续等待会重新检查发送端或接收端关闭状态。
        has_receivers_waiting.store(false, std::memory_order_seq_cst);
      }
      /// @brief 队列锁外恢复已摘除的发送等待者，恢复后由发送逻辑检查关闭状态。
      detail::wake_all(send_nodes);
      /// @brief 队列锁外恢复已摘除的接收等待者，结束本次等待而不阻塞工作线程。
      detail::wake_all(recv_nodes);
    }

    std::vector<cell> cells;  ///< 创建时一次性分配的固定容量环形存储。
    mutable std::mutex single_mutex;  ///< 容量为 1 时保护单槽，不能使用序号环算法。
    std::optional<T> single_value;  ///< 容量为 1 时实际的消息存储。
    std::atomic<bool> single_full{false};  ///< 单槽是否占用，等待登记检查使用 acquire。
    alignas(64) std::atomic<std::size_t> tail{0};  ///< 多生产者竞争的下一个写入位置。
    alignas(64) std::atomic<std::size_t> head{0};  ///< 唯一消费者的下一个读取位置，分离缓存行。
    std::atomic<std::size_t> senders{1};  ///< 发送侧控制块是否存活（1/0），不再是端点副本数量。
    std::atomic_flag receiver_busy =
        ATOMIC_FLAG_INIT;  ///< 阻止多个未完成 recv 同时使用单消费者状态。
    std::atomic<bool> receiver_open{true};  ///< 接收端已关闭时拒绝新的发送和接收。
    std::mutex wait_mutex;  ///< 只在背压、空等待、取消及关闭时保护等待链表。
    detail::wait_queue waiting_senders,  ///< 因通道满而挂起的发送者 FIFO，访问受等待队列锁保护。
        waiting_receivers;  ///< 因通道空而挂起的接收者 FIFO，访问受等待队列锁保护。
    std::atomic<bool> has_senders_waiting{false},  ///< 发送等待队列的原子非空提示，配合登记后复查防止丢失通知。
        has_receivers_waiting{false};  ///< 接收等待队列的原子非空提示，配合登记后复查防止丢失通知。
  };

  /// @brief 所有 sender 副本与未完成 send 共享同一个发送侧生命期控制块。
  /// 最后一个引用消失时关闭发送侧；每条消息只增减 shared_ptr 引用，
  /// 不再同时修改 state::senders，避免多生产者反复争用第二条计数缓存行。
  struct sender_lifetime {
    std::shared_ptr<state> shared;  ///< 持有队列状态，接收侧可以比发送侧活得更久。

    explicit sender_lifetime(std::shared_ptr<state> value) : shared(std::move(value)) {}

    /// @brief 只在最后一个 sender/send 操作释放时执行。
    ~sender_lifetime() { shared->drop_sender(); }
  };

  /// @brief 此引用使用 sender_lifetime 的控制块，别名指针仍直接指向 state。
  /// 因此异步 send 即使在 sender.close 后才开始，也持有发送侧开放租约。
  struct sender_lease {
    std::shared_ptr<state> shared;  ///< 别名 shared_ptr，拥有的是发送侧控制块。

    explicit sender_lease(std::shared_ptr<state> value) : shared(std::move(value)) {}

    sender_lease(sender_lease&& other) noexcept : shared(std::move(other.shared)) {}

    sender_lease(const sender_lease&) = delete;
  };

  struct send_waiter {
    state& shared;  ///< 关联操作的状态引用或指针，使用期间须由外部所有者保持存活。
    detail::wait_node node;  ///< 嵌入对象的稳定等待或分派节点，入队后直到完成交接前不得移动。

    /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
    bool await_ready() const noexcept { return false; }

    /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
    bool await_suspend(std::coroutine_handle<> h) {
      {
        /// @brief 把等待登记与关闭、通知、取消放入同一队列串行边界。
        std::lock_guard lock(shared.wait_mutex);
        /// @brief 入队前在等待锁内重查接收端是否关闭或已有空间，满足任一条件就无需挂起。
        if (!shared.receiver_open.load(std::memory_order_acquire) || shared.has_space())
          return false;
        /// @brief 先记录续体与登记线程调度器，通知或取消到来时才能回到正确运行时。
        node.capture(h);
        /// @brief 把帧内发送节点登记到队尾，队列仅借用节点地址。
        shared.waiting_senders.push(&node);
        /// @brief 先公布发送等待者存在，再复查空闲槽位，与释放槽位方的全序栅栏配对。
        shared.has_senders_waiting.store(true, std::memory_order_seq_cst);
        /// @brief 和消费侧的发布栅栏配对：登记后重查，避免释放与入队交错时睡过头。
        std::atomic_thread_fence(std::memory_order_seq_cst);
        /// @brief 公布等待提示并执行栅栏后再次发现空间时撤回节点，不依赖未来另一条通知。
        if (shared.has_space()) {
          /// @brief 当前线程仍持等待锁，可安全撤回尚未交接的发送等待节点。
          shared.waiting_senders.remove(&node);
          /// @brief 撤回后同步修正发送等待提示，使通知方只为真实剩余等待者取得队列锁。
          shared.has_senders_waiting.store(shared.waiting_senders.head != nullptr,
                                           std::memory_order_seq_cst);
          return false;
        }
      }
      /// @brief 锁外注册停止回调，并让取消摘链时同时修正发送等待提示。
      node.register_stop(shared.wait_mutex, shared.waiting_senders, &shared.has_senders_waiting);
      /// @brief 用原子状态交接挂起资格；通知或取消已先到达时由当前执行流直接继续。
      return node.arm();
    }

    /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
    void await_resume() const {
      /// @brief 恢复后以节点唯一终态判断是否取消，通知先胜出时不把迟到停止解释为取消。
      if (node.cancelled())
        /// @brief 在等待协程上报告协作取消，节点已安全离队而不会再被通知方借用。
        throw operation_cancelled{};
    }
  };

  struct recv_waiter {
    state& shared;  ///< 关联操作的状态引用或指针，使用期间须由外部所有者保持存活。
    detail::wait_node node;  ///< 嵌入对象的稳定等待或分派节点，入队后直到完成交接前不得移动。

    /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
    bool await_ready() const noexcept { return false; }

    /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
    bool await_suspend(std::coroutine_handle<> h) {
      {
        /// @brief 把等待登记与关闭、通知、取消放入同一队列串行边界。
        std::lock_guard lock(shared.wait_mutex);
        /// @brief 在登记接收等待前重新检查关闭、最后发送端退出或已有消息，避免本可继续却进入等待。
        if (!shared.receiver_open.load(std::memory_order_acquire)
            || shared.senders.load(std::memory_order_acquire) == 0 || shared.has_value())
          return false;
        /// @brief 先记录续体与登记线程调度器，通知或取消到来时才能回到正确运行时。
        node.capture(h);
        /// @brief 把帧内接收节点登记到队尾，后续通知与取消通过同一锁竞争摘除权。
        shared.waiting_receivers.push(&node);
        /// @brief 先公布接收等待者存在，再复查已发布消息，与生产者通知顺序配对。
        shared.has_receivers_waiting.store(true, std::memory_order_seq_cst);
        /// @brief 和生产侧的发布栅栏配对：消息已发布则撤回等待，不依赖共享 epoch。
        std::atomic_thread_fence(std::memory_order_seq_cst);
        /// @brief 公布等待提示后的消息复查成功时撤回节点，避免通知恰好发生在登记窗口时睡过头。
        if (shared.has_value()) {
          /// @brief 等待锁内撤回当前接收节点，后续消费直接重试真实环形槽状态。
          shared.waiting_receivers.remove(&node);
          /// @brief 撤回后修正接收等待提示，不能把仍有其他等待者的队列误报为空。
          shared.has_receivers_waiting.store(shared.waiting_receivers.head != nullptr,
                                             std::memory_order_seq_cst);
          return false;
        }
      }
      /// @brief 离开等待锁后登记停止回调；已停止令牌可同步执行取消摘链而不会递归获取同一锁。
      node.register_stop(
          shared.wait_mutex, shared.waiting_receivers, &shared.has_receivers_waiting);
      /// @brief 用原子状态交接挂起资格；通知或取消已先到达时由当前执行流直接继续。
      return node.arm();
    }

    /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
    void await_resume() const {
      /// @brief 恢复后以节点唯一终态判断是否取消，通知先胜出时不把迟到停止解释为取消。
      if (node.cancelled())
        /// @brief 在等待协程上报告协作取消，节点已安全离队而不会再被通知方借用。
        throw operation_cancelled{};
    }
  };

  static task<expected<void>> send_impl(sender_lease lease, T value) {
    /// @brief 发送操作持有发送端别名租约，即使公开 sender 已关闭，该操作仍保持发送侧生命期。
    auto& shared = *lease.shared;
    for (;;) {
      /// @brief 每次重试前观察接收端关闭，关闭后不能再向通道发布新消息。
      if (!shared.receiver_open.load(std::memory_order_acquire))
        co_return std::unexpected(make_error(Error::ClosedChannel));
      /// @brief 尝试真实无锁发布；失败时 value 尚未被移出，可以安全登记背压等待后重试。
      if (shared.try_push(value))
        co_return expected<void>{};
      /// @brief 通道暂满时挂起协程，恢复只代表应重新检查空间与关闭条件。
      co_await send_waiter{shared, {}};
    }
  }

  static task<expected<T>> recv_impl(std::shared_ptr<state> shared) {
    /// @brief 接收端没有有效共享状态时直接报告通道关闭，不访问已释放的队列。
    if (!shared)
      co_return std::unexpected(make_error(Error::ClosedChannel));
    for (;;) {
      /// @brief 接收端显式关闭后终止当前收取操作，不能继续使用唯一消费者状态。
      if (!shared->receiver_open.load(std::memory_order_acquire))
        co_return std::unexpected(make_error(Error::ClosedChannel));
      /// @brief 先尝试取出已经发布的消息，最后发送端退出后仍允许把已有消息排空。
      if (auto value = shared->try_pop())
        co_return std::move(*value);
      /// @brief 队列已空且最后发送端已经退出时，确定后续不再有消息并报告通道结束。
      if (shared->senders.load(std::memory_order_acquire) == 0)
        co_return std::unexpected(make_error(Error::ClosedChannel));
      /// @brief 空通道等待消息或关闭通知，恢复后重新检查真实槽状态以处理竞争与虚假机会。
      co_await recv_waiter{*shared, {}};
    }
  }

 public:
  /// @brief awaiter 本体位于调用者协程帧。无竞争路径直接尝试环形队列，
  /// 只有满/空时才构造额外的 task 帧用于“醒来后再试”的循环。
  class send_operation {
   public:
    send_operation(std::shared_ptr<state> shared, T value)
        : lease_(std::move(shared)), value_(std::move(value)) {}

    send_operation(send_operation&&) noexcept = default;

    send_operation(const send_operation&) = delete;

    /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
    bool await_ready() {
      if (!lease_.shared || !lease_.shared->receiver_open.load(std::memory_order_acquire)) {
        /// @brief 立即记录通道关闭错误，快速等待器无需再为确定失败分配慢路径子协程。
        result_ = std::unexpected(make_error(Error::ClosedChannel));
        return true;
      }
      return lease_.shared->try_push(value_);
    }

    /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
    template <class Parent>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Parent> parent) {
      /// @brief 快速发送未完成时才创建背压慢路径任务，连同消息和发送侧生命期一起移入其帧。
      auto slow = send_impl(std::move(lease_), std::move(value_));
      /// @brief 慢路径确实需要执行时才保存其独占等待器，快速成功不构造额外子协程帧。
      slow_.emplace(std::move(slow).operator co_await());
      return slow_->await_suspend(parent);
    }

    /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
    expected<void> await_resume() {
      /// @brief 存在慢路径时由其等待器领取结果与释放子帧，否则读取当前对象保存的快速结果。
      if (slow_)
        return slow_->await_resume();
      return std::move(result_);
    }

   private:
    sender_lease lease_;  ///< 发送端共享租约，控制通道何时不再可能产生新消息。
    T value_;  ///< 持有当前结果值或快照，读取前须确认操作已完成。
    expected<void> result_{};  ///< 本次操作的完成结果，包含成功状态或错误信息。
    std::optional<typename task<expected<void>>::awaiter> slow_;  ///< 仅在慢路径创建的子协程等待器，快速成功时不分配子协程帧。
  };

  class recv_operation {
   public:
    explicit recv_operation(std::shared_ptr<state> shared)
        : shared_(std::move(shared)), busy_(!!shared_) {
      if (busy_ && shared_->receiver_busy.test_and_set(std::memory_order_acquire))
        throw std::logic_error("mpsc 只允许一个未完成的 recv");
    }

    recv_operation(recv_operation&& other) noexcept
        : shared_(std::move(other.shared_)),
          busy_(std::exchange(other.busy_, false)),
          value_(std::move(other.value_)),
          slow_(std::move(other.slow_)) {}

    recv_operation(const recv_operation&) = delete;

    ~recv_operation() {
      if (busy_)
        /// @brief 唯一接收操作结束后归还消费者占用标记，允许下一次 recv 使用无锁 head 状态。
        shared_->receiver_busy.clear(std::memory_order_release);
    }

    /// @brief 检查等待是否已经满足；返回 true 时编译器跳过挂起登记。
    bool await_ready() {
      if (!shared_)
        return true;
      if (!shared_->receiver_open.load(std::memory_order_acquire))
        return true;
      /// @brief 在分配慢路径之前先尝试领取消息，已有数据时保持收取快路径无需子帧。
      if (auto item = shared_->try_pop())
        /// @brief 把已领取消息移入当前等待器结果槽，原队列槽位随后可被生产者复用。
        value_.emplace(std::move(*item));
      return value_.has_value() || shared_->senders.load(std::memory_order_acquire) == 0;
    }

    /// @brief 登记恢复目标，并按当前等待协议交接挂起与完成责任。
    template <class Parent>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Parent> parent) {
      /// @brief 只有队列暂空且仍可能有新消息时才创建异步等待的慢路径子协程。
      auto slow = recv_impl(shared_);
      /// @brief 慢路径确实需要执行时才保存其独占等待器，快速成功不构造额外子协程帧。
      slow_.emplace(std::move(slow).operator co_await());
      return slow_->await_suspend(parent);
    }

    /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
    expected<T> await_resume() {
      /// @brief 存在慢路径时由其等待器领取结果与释放子帧，否则读取当前对象保存的快速结果。
      if (slow_)
        return slow_->await_resume();
      /// @brief 快速路径已经取得消息时直接移动交付，不再重复访问队列或构造等待。
      if (value_)
        return std::move(*value_);
      if (shared_ && !shared_->receiver_open.load(std::memory_order_acquire))
        return std::unexpected(make_error(Error::ClosedChannel));
      return std::unexpected(make_error(Error::ClosedChannel));
    }

   private:
    std::shared_ptr<state> shared_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
    bool busy_{};  ///< 当前对象是否已有互斥执行中的操作。
    std::optional<T> value_;  ///< 持有当前结果值或快照，读取前须确认操作已完成。
    std::optional<typename task<expected<T>>::awaiter> slow_;  ///< 仅在慢路径创建的子协程等待器，快速成功时不分配子协程帧。
  };

  class sender {
   public:
    explicit sender(std::shared_ptr<state> shared) : shared_(std::move(shared)) {}

    ~sender() { close(); }

    /// @brief 所有端点共享发送侧控制块，复制不会单独修改队列状态计数。
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
      if (this != &other) {
        close();
        shared_ = std::move(other.shared_);
      }
      return *this;
    }

    /// @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。
    void close() {
      /// @brief 最后一个端点或未结束 send 才会触发 sender_lifetime 析构。
      shared_.reset();
    }

    /// @brief 非协程入口先复制状态；调用者随后销毁 sender 也不会悬空 this。
    send_operation send(T value) const { return send_operation{shared_, std::move(value)}; }

    /// @brief 满队列返回 false；端点关闭返回 ClosedChannel。快路径不进入等待锁。
    expected<bool> try_send(T value) const {
      if (!shared_ || !shared_->receiver_open.load(std::memory_order_acquire))
        return std::unexpected(make_error(Error::ClosedChannel));
      return shared_->try_push(value);
    }

   private:
    std::shared_ptr<state> shared_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
  };

  class receiver {
   public:
    explicit receiver(std::shared_ptr<state> shared) : shared_(std::move(shared)) {}

    ~receiver() { close(); }

    receiver(const receiver&) = delete;

    receiver& operator=(const receiver&) = delete;

    receiver(receiver&& other) noexcept : shared_(std::move(other.shared_)) {}

    receiver& operator=(receiver&& other) noexcept {
      if (this != &other) {
        close();
        shared_ = std::move(other.shared_);
      }
      return *this;
    }

    /// @brief 请求结束资源使用，并按所属组件契约排空已经接受的操作。
    void close() {
      if (shared_) {
        /// @brief 最后接收端关闭时发布关闭并摘除两类等待链，令所有在途等待重新检查结束条件。
        shared_->drop_receiver();
        /// @brief 归还当前端点的共享状态租约，已经接受的异步操作仍持有各自独立租约。
        shared_.reset();
      }
    }

    /// @brief 发起一次读取或接收，返回值保留短读、EOF 与错误的区别。
    recv_operation recv() const { return recv_operation{shared_}; }

    expected<std::optional<T>> try_recv() const {
      if (!shared_ || !shared_->receiver_open.load(std::memory_order_acquire))
        return std::unexpected(make_error(Error::ClosedChannel));
      /// @brief 同步试取也必须独占唯一消费者标志，重叠接收不能同时推进 head。
      if (shared_->receiver_busy.test_and_set(std::memory_order_acquire))
        throw std::logic_error("mpsc 只允许一个未完成的 recv");

      struct release_busy {
        state& shared;  ///< 关联操作的状态引用或指针，使用期间须由外部所有者保持存活。

        ~release_busy() { shared.receiver_busy.clear(std::memory_order_release); }
      } cleanup{*shared_};

      /// @brief 持有唯一消费者资格后先领取已发布消息，再判断发送侧是否结束。
      if (auto value = shared_->try_pop())
        return std::move(value);
      /// @brief 实际队列为空且发送侧已结束时返回通道关闭，否则仅报告暂时无消息。
      if (shared_->senders.load(std::memory_order_acquire) == 0)
        return std::unexpected(make_error(Error::ClosedChannel));
      return std::optional<T>{};
    }

   private:
    std::shared_ptr<state> shared_;  ///< 共享持有操作状态，使异步执行与结果领取期间地址保持稳定。
  };

  static std::pair<sender, receiver> make(std::size_t capacity) {
    /// @brief 通道必须至少具有一个实际槽位，零容量不支持缓冲收发语义。
    if (capacity == 0)
      throw std::invalid_argument("mpsc 容量必须大于零");
    /// @brief 一次性分配固定容量环形状态，后续普通收发不因队列扩容分配内存。
    auto shared = std::make_shared<state>(capacity);
    /// @brief 每个 channel 一次发送侧控制块分配；热路径不会额外分配租约对象。
    auto lifetime = std::make_shared<sender_lifetime>(shared);
    /// @brief 建立指向同一队列的别名租约，发送端副本计数与队列状态本身的生命期分别管理。
    std::shared_ptr<state> send_state{std::move(lifetime), shared.get()};
    return {sender{std::move(send_state)}, receiver{std::move(shared)}};
  }
};
}
#endif
