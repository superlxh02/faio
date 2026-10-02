#ifndef FAIO_DETAIL_COROUTINE_JOIN_HANDLE_HPP
#define FAIO_DETAIL_COROUTINE_JOIN_HANDLE_HPP

#include "faio/detail/coroutine/coroutine_wait.hpp"
#include "faio/detail/coroutine/root_task.hpp"
#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <variant>

namespace faio {
namespace detail {
// 统一结果存储类型：void 用 monostate 表示正常完成，其余类型保存用户值。
template <class T> using handle_value = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

// 父 stop_token 的回调只发出请求；实际取消由正在等待的 operation 处理。
// 父停止源到子停止源的连接回调，只转发请求，不执行任务销毁。
struct forward_stop {
  // 借用目标停止源；所属共享状态/作用域保证它活到回调注销。
  std::stop_source* source;
  // 父令牌停止时向目标源发出同样的协作请求。
  void operator()() const noexcept { source->request_stop(); }
};

// 根协程与唯一结果句柄共同持有的完成状态。
// 结果发布后不再修改 value/error，ready 和 completion 建立可见性。
template <class T> struct join_handle_state {
  // 正常完成的结果；void 使用占位值，失败时保持为空。
  std::optional<handle_value<T>> value;
  // 用户 task 抛出的异常，消费结果时重新抛出。
  std::exception_ptr error;
  // 供协程等待者使用的完成事件，不阻塞 worker。
  ::faio::detail::completion_event completion;
  // 供普通线程 done/wait 使用的完成标志，发布后不复位。
  std::atomic<bool> ready{false};
  // 结果是否已经领取，包含已观察并重抛异常的情况，限制单次消费。
  std::atomic<bool> consumed{false};
  // 消费权是否已被丢弃，配合发布路径处理无人观察的异常。
  std::atomic<bool> abandoned{false};
  // 子任务独立停止源，request_stop 与父停止转发都操作它。
  std::stop_source stop_source;
  // 可选父停止回调，保存在共享状态内以覆盖整个子任务生命周期。
  std::optional<std::stop_callback<forward_stop>> parent_callback;

  // 在结果/异常写入后发布完成，同时唤醒普通线程与协程等待者。
  void publish() {
    // release 发布 value/error，普通线程 acquire 读取 ready 后可以取结果。
    ready.store(true, std::memory_order_release);
    // 解除外部线程的 atomic::wait 阻塞，不涉及 worker 阻塞等待。
    ready.notify_all();
    // 恢复通过 co_await JoinHandle 登记的协程等待者。
    completion.notify();
    // 若结果已无人领取且任务失败，终止进程，避免静默吞异常。
    if (abandoned.load(std::memory_order_acquire) && error) std::terminate();
  }
  // 放弃唯一消费权；不会请求停止，也不会立刻销毁运行中的任务。
  void abandon() noexcept {
    // 已经领取结果或观察异常后，句柄析构无需再标记放弃。
    if (consumed.load(std::memory_order_acquire)) return;
    // 让仍在运行的发布方知道结果已无人观察。
    abandoned.store(true, std::memory_order_release);
    // 若完成先于放弃，当前线程负责处理已有的未观察异常。
    if (ready.load(std::memory_order_acquire) && error) std::terminate();
  }
  // 领取一次结果或观察异常；调用前必须已通过 wait/完成事件确认任务完成。
  T take() {
    // 抢占一次性消费权，重复领取时报告逻辑错误。
    if (consumed.exchange(true, std::memory_order_acq_rel))
      // 拒绝二次读取，避免从已移动的结果中再次取值。
      throw std::logic_error("JoinHandle 结果只能消费一次");
    // 在已标记 consumed 后重抛，异常被捕获也算已经观察。
    if (error) std::rethrow_exception(error);
    // 有结果时移动返回用户值；void 消费只完成状态检查。
    if constexpr (!std::is_void_v<T>) return std::move(value.value());
  }
};

// 包装用户 task 的根协程：捕获结果/异常并发布共享状态。
// state 的 shared_ptr 使句柄先析构时状态仍存活，tracker 仅借用外层任务组。
template <class T>
detached_task observed_coro(
    task<T> child, std::shared_ptr<join_handle_state<T>> state,
    task_tracker* tracker) {
  // 建立当前根任务的任务组，供用户 task 及其派生任务继承。
  ::faio::detail::current_tracker = tracker;
  // 安装子任务独立停止令牌，随后 co_await 的 task 会继承它。
  ::faio::detail::current_stop_token = state->stop_source.get_token();
  try {
    // 编译期选择无结果 task 的存储路径。
    if constexpr (std::is_void_v<T>) {
      // 消费并启动用户 task，等待其结束。
      co_await std::move(child);
      // 无结果 task 正常结束后保存成功占位。
      state->value.emplace();
    } else {
      // 等待有结果 task，结果在完成发布前写入共享状态。
      state->value.emplace(co_await std::move(child));
    }
  // 把用户异常和结果存储异常交给句柄观察，不让根协程直接 terminate。
  } catch (...) { state->error = std::current_exception(); }
  // 发布已写好的结果或异常；根协程随后析构并归还任务计数。
  state->publish();
}

// 立即启动可观察结果的任务，返回供 JoinHandle 持有的共享状态。
// 父令牌只转发停止请求；没有 tracker 时任务仍登记到运行时活动根任务计数。
template <class T>
std::shared_ptr<join_handle_state<T>> start_observed(
    scheduler_ref scheduler, task<T> child, task_tracker* tracker,
    std::stop_token parent_stop,
    task_lifetime_ref lifetime = current_task_lifetime()) {
  // 分配和登记前拒绝空调度器，避免提交到不存在的运行时。
  if (!scheduler) throw std::logic_error("没有可用的运行时");
  // 创建跨根协程与句柄共享的结果对象；这是 spawn 的共享状态分配。
  auto state = std::make_shared<join_handle_state<T>>();
  // 父任务不可取消时跳过回调登记。
  if (parent_stop.stop_possible())
    // 父请求转发到子停止源；父已停止时构造回调会同步转发。
    state->parent_callback.emplace(parent_stop, forward_stop{&state->stop_source});
  // 先构造根帧并转移 child 所有权，尚未修改任务计数。
  auto root = observed_coro(std::move(child), state, tracker);
  // 提交前登记外层任务组，根 promise 析构负责归还。
  if (tracker) tracker->add();
  // 登记运行时根任务并入队，提交失败由根帧清理计数。
  start_detached(std::move(root), scheduler, tracker, lifetime);
  // 把共享状态交给结果句柄，执行中的根帧保留另一份引用。
  return state;
}
} // namespace detail

// 移动独占的任务结果句柄。丢弃句柄不会停止任务；未观察的异常会终止程序，
// 与无结果后台任务的旧行为一致。普通线程可以 wait/get，worker 必须 co_await。
// 用户持有的移动独占消费权；共享状态的引用计数只管理状态生命周期。
// 不能并发操作同一个句柄对象，普通线程等待与协程等待均只能领取一次结果。
template <class T> class [[nodiscard]] join_handle {
public:
  // 接管启动入口返回的状态引用，构造本身不再启动任务。
  explicit join_handle(std::shared_ptr<detail::join_handle_state<T>> state)
      : state_(std::move(state)) {}
  // 移动消费权到新句柄，原句柄变为空。
  join_handle(join_handle&&) noexcept = default;
  // 禁止复制消费权，避免多个句柄领取同一个结果。
  join_handle(const join_handle&) = delete;
  // 禁止复制赋值。
  join_handle& operator=(const join_handle&) = delete;
  // 先放弃当前消费权，再接管另一句柄；原任务不会因赋值被取消。
  join_handle& operator=(join_handle&& other) noexcept {
    // 自移动赋值时保持现有消费权。
    if (this != &other) {
      // 被覆盖的结果视为无人观察，已有未观察异常按规则终止。
      if (state_) state_->abandon();
      // 转移状态引用及唯一消费权，other 留为空。
      state_ = std::move(other.state_);
    }
    // 返回赋值后的句柄。
    return *this;
  }
  // 析构放弃未领取结果，不等待、不停止运行中的任务。
  ~join_handle() { if (state_) state_->abandon(); }

  // 立即查询完成状态；空句柄返回 false，不消费结果。
  bool done() const noexcept {
    // acquire 观察任务完成发布，避免读取未写完的状态。
    return state_ && state_->ready.load(std::memory_order_acquire);
  }
  // 对非空句柄发出协作停止请求；不保证任务此时已经结束。
  void request_stop() noexcept { if (state_) state_->stop_source.request_stop(); }
  // 返回任务停止令牌，空句柄得到不可停止的空令牌。
  std::stop_token stop_token() const noexcept {
    // 取得同一个子停止源的观察端，令牌本身不会消费结果。
    return state_ ? state_->stop_source.get_token() : std::stop_token{};
  }
  // 普通线程阻塞到任务完成，不领取结果；worker 上调用会抛逻辑错误。
  void wait() const {
    // 空句柄没有有效结果状态，拒绝等待或转交等待消费权。
    if (!state_) throw std::logic_error("空 JoinHandle");
    // 防止在调度线程上阻塞，从而饿死当前任务依赖的其他任务。
    if (::faio::detail::on_runtime_worker())
      // 协程调用方应改为 co_await，保持 worker 可以执行其他任务。
      throw std::logic_error("不能在 worker 上阻塞等待 JoinHandle");
    // 循环检查 ready；任务完成前普通线程进入原子等待。
    for (bool ready = state_->ready.load(std::memory_order_acquire); !ready;
         // 每次唤醒后重新 acquire 读取，看到结果发布后退出。
         ready = state_->ready.load(std::memory_order_acquire))
      // 阻塞普通线程等待 ready 改变；该接口不用于 worker。
      state_->ready.wait(false, std::memory_order_acquire);
  }
  // 普通线程等待完成并领取一次结果，异常在这里重抛。
  T get() {
    // 先建立完成可见性，再读取非原子的结果/异常槽位。
    wait();
    // 消费结果或观察异常；二次调用 get 会被 consumed 检查拒绝。
    return state_->take();
  }

  // JoinHandle 的协程等待者，接管句柄消费权并借用完成事件。
  struct awaiter {
    // 持有状态生命周期及唯一消费权，原 JoinHandle 转为为空。
    std::shared_ptr<detail::join_handle_state<T>> state;
    // 嵌入事件等待者，其节点直接保存在当前 awaiter/父协程帧内。
    ::faio::detail::completion_event::awaiter completion;
    // 先接管状态，再从该状态创建 completion awaiter；成员顺序不能颠倒。
    explicit awaiter(std::shared_ptr<detail::join_handle_state<T>> s)
        : state(std::move(s)), completion(state->completion.wait()) {}
    // 仅在登记前移动 awaiter，转移消费权和未登记的事件节点。
    awaiter(awaiter&&) = default;
    // 禁止复制等待者及消费权。
    awaiter(const awaiter&) = delete;
    // 未完成消费时析构遵守句柄放弃规则；已消费时 abandon 无操作。
    ~awaiter() { if (state) state->abandon(); }
    // 挂起前检查：任务已完成则立即继续取结果。
    bool await_ready() const noexcept { return completion.await_ready(); }
    // 挂起时：登记完成事件，由事件处理登记与通知竞态。
    bool await_suspend(std::coroutine_handle<> h) { return completion.await_suspend(h); }
    // 恢复时：领取一次结果或重抛任务异常。
    T await_resume() { return state->take(); }
  };
  // 右值等待时把状态从句柄转移到 awaiter，原句柄不再持有消费权。
  awaiter operator co_await() && {
    // 空句柄没有有效结果状态，拒绝等待或转交等待消费权。
    if (!state_) throw std::logic_error("空 JoinHandle");
    // 原句柄置空，等待者成为唯一结果消费者。
    return awaiter{std::exchange(state_, {})};
  }
  // lvalue 等待也转移结果消费权；等待后原句柄变为空，避免二次读取。
  // 左值等待也消费句柄；等待后不能再用原句柄 get 或 request_stop。
  awaiter operator co_await() & {
    // 空句柄没有有效结果状态，拒绝等待或转交等待消费权。
    if (!state_) throw std::logic_error("空 JoinHandle");
    // 原句柄置空，等待者成为唯一结果消费者。
    return awaiter{std::exchange(state_, {})};
  }

private:
  // 任务共享状态的引用；句柄独占结果消费权，但状态还由根帧持有。
  std::shared_ptr<detail::join_handle_state<T>> state_;
};

} // namespace faio
#endif
