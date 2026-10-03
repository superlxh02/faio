#ifndef FAIO_DETAIL_COROUTINE_COROUTINE_WAIT_HPP
#define FAIO_DETAIL_COROUTINE_COROUTINE_WAIT_HPP

#include "faio/detail/common/cancellation.hpp"
#include "faio/detail/coroutine/task_context.hpp"
#include <atomic>
#include <coroutine>
#include <mutex>
#include <optional>
#include <stop_token>

namespace faio::detail {
// 前置声明侵入式等待队列，取消回调只需保存其地址。
struct wait_queue;

// 0=正在注册、1=允许异步恢复、2=正常唤醒、3=取消。注册和通知同时发生时，
// await_suspend 自己返回 false；只有已经挂起的节点才被投递到调度器。
// 一次等待操作的登记节点，由 awaiter/协程帧拥有，队列只借用指针。
// 节点登记后不能移动或从外部销毁；必须由通知/取消完成交接，
// 恢复等待操作后再清理 awaiter，不能仅凭 queued=false 就提前销毁。
struct wait_node {
  // 等待完成后需要恢复的协程句柄，不拥有该协程帧。
  std::coroutine_handle<> handle{};
  // 登记时所属运行时的调度句柄，跨线程通知也投递回这个运行时。
  scheduler_ref scheduler{};
  // 侵入式单链表的后继指针，仅用于等待队列链接。
  wait_node* next{};
  // 标记节点是否仍由队列管理；通知与取消通过同一队列锁争夺摘除权。
  bool queued{};  // 只在拥有 wait_queue 的 mutex 保护下访问
  // 登记/挂起交接状态：0 登记中，1 允许异步恢复，2 正常通知，3 取消。
  // phase 处理发布句柄后、await_suspend 尚未返回时通知已到达的竞态。
  std::atomic<unsigned char> phase{0};

  // 停止源调用的回调，借用节点和队列设施；自身存储在节点的 callback 中。
  struct cancel_callback {
    // 本次等待的节点，回调不得在它生命周期结束后执行。
    wait_node* node;
    // 保护目标队列的外部互斥锁，正常通知和取消共用它。
    std::mutex* mutex;
    // 需要摘除本节点的等待队列，不拥有队列。
    wait_queue* queue;
    // 可选的等待者提示标志，MPSC 用它跳过无等待者时的通知锁。
    std::atomic<bool>* has_waiters;

    // 停止被请求时执行：先摘除节点、更新提示，再发布取消并安排恢复。
    void operator()() const noexcept;
  };

  // 按需就地存储停止回调；销毁时注销回调并同步仍在执行的回调。
  std::optional<std::stop_callback<cancel_callback>> callback;

  // 创建尚未登记的空节点，所有状态使用成员初值。
  wait_node() = default;

  // 仅用于登记前的 awaiter 移动：创建新的空节点，不转移地址相关状态。
  // 已经 capture/入队的节点不能靠这个构造函数迁移。
  wait_node(wait_node&&) noexcept {}

  // 禁止复制，否则一个等待操作可能出现两个节点身份。
  wait_node(const wait_node&) = delete;

  // 登记等待前捕获恢复目标；调用者随后在原语的队列锁下入队。
  void capture(std::coroutine_handle<> h) noexcept {
    // 记录需要恢复的协程，节点本身并不负责销毁它。
    handle = h;
    // 记录注册线程的调度器，而非未来通知线程的调度器。
    scheduler = current_scheduler();
  }

  // 为本次已入队等待注册当前停止令牌；必须在释放队列锁后调用。
  // stop_callback 构造可能同步执行回调，持锁构造会造成重复获取同一把锁。
  void register_stop(std::mutex& mutex,
                     wait_queue& queue,
                     std::atomic<bool>* has_waiters = nullptr);

  // 完成登记到挂起的交接：成功表示可以挂起，失败表示通知/取消已提前到达。
  bool arm() noexcept {
    // 仅允许从登记中的初始状态取得挂起资格。
    unsigned char expected = 0;
    // 发布可恢复状态；失败时 await_suspend 返回 false，由当前执行流继续。
    return phase.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
  }

  // 恢复后检查退出原因，具体原语据此抛 operation_cancelled。
  bool cancelled() const noexcept { return phase.load(std::memory_order_acquire) == 3; }

  // 发布正常通知；调用者须已取得节点的唯一摘除权。
  // 调用后协程可能立刻恢复并销毁节点，通知方不能继续访问这个节点。
  void wake() {
    // 记录正常完成，读取通知前是否已经取得挂起资格。
    auto previous = phase.exchange(2, std::memory_order_acq_rel);
    // 仅已挂起者需要重新入队；登记中收到通知由 arm 失败直接继续。
    if (previous == 1)
      scheduler.schedule(handle);
  }

  // 发布取消；与 wake 一样要求已取得唯一摘除权，不强制销毁协程。
  void cancel() {
    // 记录取消原因，恢复时由 cancelled() 观察。
    auto previous = phase.exchange(3, std::memory_order_acq_rel);
    // 仅已挂起者需要重新入队；登记中收到取消由 arm 失败直接继续。
    if (previous == 1)
      scheduler.schedule(handle);
  }
};

// 不拥有节点的 FIFO 单链表；本类型不带锁，所有操作由使用方的锁保护。
// push/pop 为 O(1)，取消或撤回时 remove 为 O(n)，只在争用慢路径使用。
struct wait_queue {
  // 最早登记的等待者；空队列为 nullptr。
  wait_node* head{};
  // 最后登记的等待者，尾插时避免遍历整条链。
  wait_node* tail{};

  // 将尚未入队的节点登记到队尾，调用时必须持有所属队列锁。
  void push(wait_node* node) noexcept {
    // 新节点将成为队尾，清除后继以结束这条链。
    node->next = nullptr;
    // 宣布节点归队列管理，取消回调据此判断是否还能摘除。
    node->queued = true;
    // 非空队列将旧队尾链接到新节点。
    if (tail)
      tail->next = node;
    // 空队列让新节点同时成为队首。
    else
      head = node;
    // 更新队尾，保留 FIFO 入队顺序。
    tail = node;
  }

  // 摘除最早等待者并转交唯一通知权；不恢复协程，也不释放节点。
  wait_node* pop() noexcept {
    // 保存旧队首，摘除后返回给通知方。
    auto* node = head;
    // 空队列直接返回 nullptr，非空队列才需要修复链接。
    if (node) {
      // 把后继提升为新队首。
      head = node->next;
      // 摘掉最后一个节点时同时清空队尾。
      if (!head)
        tail = nullptr;
      // 清除脱离队列的节点链接，便于调用者独立处理。
      node->next = nullptr;
      // 取消回调从此不能再次摘除，恢复工作由当前通知者负责。
      node->queued = false;
    }
    // 交给调用者在锁外执行 wake/cancel，避免锁内调度。
    return node;
  }

  // 摘除整个队列并返回链表，批量通知者取得每个节点的唯一恢复权。
  wait_node* take_all() noexcept {
    // 保存待处理链表头，节点及 next 链接仍由协程帧拥有。
    auto* nodes = head;
    // 队列立即变空，允许后续等待者登记到新队列。
    head = tail = nullptr;
    // 仍在队列锁内撤销全部 queued 标志，阻止取消回调重复处理。
    for (auto* node = nodes; node; node = node->next)
      node->queued = false;
    // 将摘除的链表交给锁外批量唤醒逻辑。
    return nodes;
  }

  // 撤回指定节点，供取消和登记后复查使用；调用方必须持有队列锁。
  void remove(wait_node* node) noexcept {
    // 保存前驱以修复单链表；队首的前驱为空。
    wait_node* previous = nullptr;
    // 从队首扫描目标，只在慢路径承担线性遍历成本。
    for (auto* current = head; current; current = current->next) {
      // 找到同一地址的节点后取得其摘除权。
      if (current == node) {
        // 目标在中间或队尾时，让前驱绕过目标。
        if (previous)
          previous->next = current->next;
        // 目标是队首时直接推进 head。
        else
          head = current->next;
        // 目标是队尾时修复 tail；单节点队列会得到空 tail。
        if (tail == current)
          tail = previous;
        // 清除目标的队列链接。
        current->next = nullptr;
        // 标记目标已被摘除，后续正常通知不能再取得它。
        current->queued = false;
        // 摘除完毕立即结束，不再访问被转交的节点。
        return;
      }
      // 前进扫描，同时保存下一轮使用的前驱。
      previous = current;
    }
  }
};

// 实现停止回调注册；mutex、queue 和可选提示标志必须活到等待操作结束。
inline void wait_node::register_stop(std::mutex& mutex,
                                     wait_queue& queue,
                                     std::atomic<bool>* has_waiters) {
  // 复制当前任务令牌，不受同一线程下一次切换 TLS 的影响。
  auto token = current_stop_token;
  // 不可取消任务跳过回调构造，保留简单等待路径。
  if (!token.stop_possible())
    return;
  // stop_callback 侵入式注册在 stop_state 中；节点本身位于协程帧，
  // 无需为取消路径再分配对象。若 token 已停止，构造时同步执行回调。
  // 回调直接保存在节点内；令牌已停止时会在此同步处理取消。
  callback.emplace(token, cancel_callback{this, &mutex, &queue, has_waiters});
}

// 停止请求到来时尝试接管仍在队列中的等待者。
inline void wait_node::cancel_callback::operator()() const noexcept {
  {
    // 与正常 pop/take_all 串行化，让一个节点只由一个路径摘除。
    std::lock_guard lock(*mutex);
    // 若通知已接管节点，停止请求不覆盖该通知的完成原因。
    if (!node->queued)
      return;  // 正常通知已经取得该节点，交给通知路径唤醒。
    // 从等待队列摘除目标，防止后续通知继续引用该节点。
    queue->remove(node);
    // MPSC 等使用方同步修正提示标志，保留其与通知代数的顺序协议。
    if (has_waiters)
      has_waiters->store(queue->head != nullptr, std::memory_order_seq_cst);
  }
  // 释放队列锁后安排取消恢复，避免恢复代码立即争用同一把锁。
  node->cancel();
}

// 正常恢复已经 take_all 摘除的节点链；调用方已释放队列锁。
inline void wake_all(wait_node* nodes) {
  // 逐个消费摘除链表，不重新接触原队列。
  while (nodes) {
    // 必须在唤醒前保存后继，唤醒后当前帧可能马上被其他 worker 销毁。
    auto* next = nodes->next;
    // 发布当前节点的正常完成，只调度已经取得挂起资格者。
    nodes->wake();
    // 利用预先保存的地址继续，不再读取已唤醒节点。
    nodes = next;
  }
}

// 取消已经摘除的整条等待链，屏障破坏等场景使用。
inline void cancel_all(wait_node* nodes) {
  // 逐个消费摘除链表，不重新接触原队列。
  while (nodes) {
    // 必须在唤醒前保存后继，唤醒后当前帧可能马上被其他 worker 销毁。
    auto* next = nodes->next;
    // 发布当前节点的取消原因，具体原语恢复时负责抛出异常。
    nodes->cancel();
    // 利用预先保存的地址继续，不再读取已唤醒节点。
    nodes = next;
  }
}

// 带记忆的一次性完成事件；完成等待不注册取消回调，也不从队列中撤回。
// 节点只入队一次，notify 将整条链原子替换为永久完成标记，没有 ABA/回收重试。
class completion_event {
 public:
  struct awaiter {
    completion_event& event;  // 借用目标事件，所属任务排空后才销毁。
    wait_node node;           // 帧内节点，沿用登记/挂起交接协议。

    bool await_ready() const noexcept {
      return event.waiters_.load(std::memory_order_acquire) == &completed_;
    }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
      node.capture(h);
      auto* head = event.waiters_.load(std::memory_order_acquire);
      for (;;) {
        if (head == &completed_)
          return false;
        node.next = head;
        // 发布句柄和 next；完成方 acquire 摘除后才会访问这条链。
        if (event.waiters_.compare_exchange_weak(
                head, &node, std::memory_order_acq_rel, std::memory_order_acquire))
          break;
      }
      // 通知可能已接管节点；arm 失败时当前执行流直接继续，避免重复恢复。
      return node.arm();
    }

    void await_resume() const noexcept {}
  };

  awaiter wait() noexcept { return {*this, {}}; }

  void notify() {
    // 同一次交接发布结果、摘除节点并关闭登记；此后不再访问事件本身。
    // 晚到等待者可立即销毁拥有事件的父帧，通知方只使用已摘出的节点。
    auto* nodes = waiters_.exchange(&completed_, std::memory_order_acq_rel);
    if (nodes == &completed_)
      return;  // 重复通知保持幂等。
    // 入队使用头插；反转独占链以保留原先按登记顺序投递的行为。
    wait_node* ordered = nullptr;
    while (nodes) {
      auto* next = nodes->next;
      nodes->next = ordered;
      ordered = nodes;
      nodes = next;
    }
    wake_all(ordered);
  }

 private:
  // 仅使用地址作为完成身份，从不访问或修改这个节点的内容。
  inline static wait_node completed_{};
  std::atomic<wait_node*> waiters_{nullptr};  // nullptr 空、节点链待完成、completed_ 已完成。
};
}  // namespace faio::detail
#endif
