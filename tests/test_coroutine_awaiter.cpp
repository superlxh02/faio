/**
 * @file test_coroutine_awaiter.cpp
 * @brief 原生等待器的值类别、恢复协议和对象生命周期回归测试。
 */
#include <faio/detail/coroutine/context/context_scope.hpp>
#include <faio/detail/coroutine/context/execution_binding.hpp>
#include <faio/detail/coroutine/launch/root_task.hpp>
#include <faio/detail/coroutine/task.hpp>
#include <gtest/gtest.h>
#include <array>
#include <coroutine>
#include <deque>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace detail = faio::detail;

/** @brief 测试驱动仅登记任务，由调用方显式取出并恢复。 */
struct ready_queue {
  std::deque<std::coroutine_handle<>> pending;  ///< 未恢复的句柄。
  int submissions{};                            ///< 成功登记次数。

  void enqueue(std::coroutine_handle<> handle) {
    pending.push_back(handle);
    ++submissions;
  }

  void enqueue_no_stealing(std::coroutine_handle<> handle) { enqueue(handle); }

  std::coroutine_handle<> pop() {
    const auto handle = pending.front();
    pending.pop_front();
    return handle;
  }
};

/** @brief 恢复线程安装独立的 worker 服务和初始任务上下文副本。 */
void resume_on(ready_queue& queue,
               std::coroutine_handle<> handle,
               const detail::task_context& initial_context,
               std::size_t worker) {
  detail::scoped_thread_context thread;
  int local_state{};
  detail::execution_thread_binding<ready_queue> binding{
      faio::basic_scheduler_ref<ready_queue>{queue}, {}, local_state, worker};
  detail::execution_thread_guard attached{binding};
  detail::scoped_task_context initial{initial_context};
  handle.resume();
}

enum class suspension { ready, immediate, queued, queued_void, transferred };

struct observation {
  const void* constructor_address{};  ///< 刚构造的对象地址。
  const void* resumed_address{};      ///< 实际执行 await_resume 的对象地址。
  int destroyed{};                    ///< 等待器析构次数。
  int attempted{};                    ///< 底层 await_suspend 调用次数。
  int transfers{};                    ///< 对称转移目标运行次数。
  int factories{};                    ///< 原生转换调用次数。
  int unused_conversion{};            ///< 优先使用成员转换时，此计数必须为零。
  int after_expression{};             ///< 完整表达式后观察到的析构次数。
  int result{};                       ///< 完整表达式的结果。
  bool throws{};                      ///< 启用 await_resume 异常。
  std::size_t resume_worker{};        ///< 实际恢复协程的 worker。
  detail::task_context resumed;       ///< await_resume 读取到的任务上下文副本。
};

/** @brief 实际执行转移目标后，再把原等待任务提交给测试驱动。 */
detail::root_task transfer_to_queue(ready_queue& queue,
                                    observation& seen,
                                    std::coroutine_handle<> awaiting) {
  ++seen.transfers;
  queue.enqueue(awaiting);
  co_return;
}

/** @brief 无法复制或移动的原生等待对象，可选择任一种标准挂起协议。 */
template <suspension Mode>
struct immovable {
  observation& seen;  ///< 外部观察记录在本对象之前创建。

  explicit immovable(observation& report) : seen(report) { seen.constructor_address = this; }

  immovable(const immovable&) = delete;
  immovable(immovable&&) = delete;

  ~immovable() { ++seen.destroyed; }

  bool await_ready() const noexcept { return Mode == suspension::ready; }

  auto await_suspend(std::coroutine_handle<> handle) const {
    ++seen.attempted;
    if constexpr (Mode == suspension::ready || Mode == suspension::immediate) {
      return false;
    } else {
      auto& queue = *detail::require_current_scheduler_for<ready_queue>().native_handle();
      if constexpr (Mode == suspension::transferred) {
        return transfer_to_queue(queue, seen, handle).release_handle();
      } else {
        queue.enqueue(handle);
        if constexpr (Mode == suspension::queued)
          return true;
      }
    }
  }

  int await_resume() const {
    seen.resumed_address = this;
    seen.resumed = detail::capture_task_context();
    seen.resume_worker = detail::current_worker_id();
    if (seen.throws)
      throw std::runtime_error("awaiter result error");
    return 42;
  }
};

/** @brief 同时提供成员转换和 ADL 转换，验证转换优先级及不可移动值的复制消除。 */
struct member_factory {
  observation& seen;

  immovable<suspension::ready> operator co_await() && {
    ++seen.factories;
    return immovable<suspension::ready>{seen};
  }

  [[maybe_unused]] friend immovable<suspension::ready> operator co_await(member_factory&& input) {
    ++input.seen.unused_conversion;
    return immovable<suspension::ready>{input.seen};
  }
};

/** @brief 单独通过 ADL 返回一个不可移动值。 */
struct adl_factory {
  observation& seen;

  friend immovable<suspension::ready> operator co_await(adl_factory&& input) {
    ++input.seen.factories;
    return immovable<suspension::ready>{input.seen};
  }
};

/** @brief 对象仍归调用者持有，转换返回稳定左值借用。 */
struct member_reference {
  immovable<suspension::ready>& target;

  auto& operator co_await() && {
    ++target.seen.factories;
    return target;
  }
};

/** @brief 检查 ADL 返回无法构造成值对象的 const 右值引用。 */
struct adl_reference {
  const immovable<suspension::ready>& target;

  friend const immovable<suspension::ready>&& operator co_await(adl_reference&& input) {
    ++input.target.seen.factories;
    return std::move(input.target);
  }
};

using test_task = faio::task<void, ready_queue>;
using root_task = detail::root_task;

template <class Coroutine, suspension Mode>
Coroutine wait_temporary(observation& seen) {
  seen.result = co_await immovable<Mode>{seen};
  seen.after_expression = seen.destroyed;
}

/** @brief 直接等待和转换后等待都应保留借用目标直到原始作用域结束。 */
template <class Coroutine>
Coroutine wait_named(std::array<observation, 5>& inputs) {
  immovable<suspension::ready> named{inputs[0]};
  const immovable<suspension::ready> const_named{inputs[1]};
  const immovable<suspension::ready> const_rvalue{inputs[2]};
  immovable<suspension::ready> member_target{inputs[3]};
  const immovable<suspension::ready> adl_target{inputs[4]};
  inputs[0].result = co_await named;
  inputs[1].result = co_await const_named;
  inputs[2].result = co_await std::move(const_rvalue);
  inputs[3].result = co_await member_reference{member_target};
  inputs[4].result = co_await adl_reference{adl_target};
  for (auto& input : inputs)
    input.after_expression = input.destroyed;
}

template <class Coroutine>
Coroutine wait_factories(std::array<observation, 2>& inputs) {
  inputs[0].result = co_await member_factory{inputs[0]};
  inputs[0].after_expression = inputs[0].destroyed;
  inputs[1].result = co_await adl_factory{inputs[1]};
  inputs[1].after_expression = inputs[1].destroyed;
}

enum class move_event {
  created,
  moved,
  resumed,
  destroy_owner,
  destroy_original,
  after_expression
};

/** @brief 检查适配器仍持有可移动右值，且析构顺序保持不变。 */
struct movable {
  std::vector<move_event>& events;
  bool original{true};

  explicit movable(std::vector<move_event>& audit) : events(audit) {
    events.push_back(move_event::created);
  }

  movable(movable&& other) noexcept : events(other.events), original(false) {
    events.push_back(move_event::moved);
  }

  ~movable() {
    events.push_back(original ? move_event::destroy_original : move_event::destroy_owner);
  }

  bool await_ready() const noexcept { return true; }

  void await_suspend(std::coroutine_handle<>) const noexcept {}

  void await_resume() const { events.push_back(move_event::resumed); }
};

template <class Coroutine>
Coroutine wait_movable(std::vector<move_event>& events) {
  co_await movable{events};
  events.push_back(move_event::after_expression);
}

/** @brief 根任务的异常必须在自身边界内观察，普通 task 的异常在消费结果时重抛。 */
root_task wait_throwing_root(observation& seen) {
  try {
    (void)co_await immovable<suspension::queued>{seen};
    ADD_FAILURE() << "expected await_resume exception";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "awaiter result error");
    seen.after_expression = seen.destroyed;
  }
}

template <class Coroutine>
class driver {
 public:
  driver()
      : owner_(std::make_shared<detail::cancellation_state>(std::stop_token{}, nullptr)),
        expected_{
            .scope = &scope_,
            .stop_token = owner_->source.get_token(),
            .cancellation_policy = detail::cancellation_error_policy::normal_if_stop_requested,
            .scheduling = {.no_stealing = true},
            .cancellation_owner = owner_},
        unrelated_{.stop_token = other_source_.get_token(), .cancellation_owner = {}} {}

  void start(Coroutine operation) {
    std::coroutine_handle<> initial;
    if constexpr (std::is_same_v<Coroutine, root_task>) {
      operation.set_context(expected_);
      scope_.add();
      {
        detail::scoped_thread_context thread;
        int local_state{};
        const faio::basic_scheduler_ref<ready_queue> scheduler{queue};
        detail::execution_thread_binding<ready_queue> binding{scheduler, {}, local_state, 0};
        detail::execution_thread_guard attached{binding};
        detail::scoped_task_context active{expected_};
        detail::schedule_task(std::move(operation), scheduler, &scope_, {});
      }
      initial = queue.pop();
      queue.submissions = 0;
    } else {
      task_frame_ = operation.take();
      task_frame_.promise().context = expected_;
      initial = task_frame_;
    }
    resume_on(queue, initial, expected_, 0);
  }

  void resume_cross_thread() {
    ASSERT_EQ(queue.pending.size(), 1U);
    const auto next = queue.pop();
    const std::jthread worker{[this, next] { resume_on(queue, next, unrelated_, 1); }};
  }

  void check_context(const observation& seen, std::size_t worker) const {
    EXPECT_EQ(seen.resumed.scope, expected_.scope);
    EXPECT_EQ(seen.resumed.stop_token, expected_.stop_token);
    EXPECT_EQ(seen.resumed.cancellation_policy, expected_.cancellation_policy);
    EXPECT_EQ(seen.resumed.scheduling, expected_.scheduling);
    EXPECT_EQ(seen.resumed.cancellation_owner, expected_.cancellation_owner);
    EXPECT_EQ(seen.resume_worker, worker);
  }

  void check_finished() const {
    EXPECT_TRUE(queue.pending.empty());
    if constexpr (std::is_same_v<Coroutine, root_task>)
      EXPECT_EQ(scope_.pending.load(std::memory_order_acquire), 0U);
    else
      EXPECT_TRUE(task_frame_.done());
  }

  void consume_result() const {
    if constexpr (std::is_same_v<Coroutine, test_task>)
      task_frame_.promise().take_result();
  }

  ~driver() {
    if (task_frame_)
      task_frame_.destroy();
  }

  ready_queue queue;

 private:
  test_task::handle_type task_frame_{};
  detail::task_tracker scope_;
  std::shared_ptr<detail::cancellation_state> owner_;
  std::stop_source other_source_;
  const detail::task_context expected_;
  const detail::task_context unrelated_;
};

template <class Coroutine, suspension Mode>
void check_temporary() {
  observation seen;
  driver<Coroutine> run;
  run.start(wait_temporary<Coroutine, Mode>(seen));
  constexpr bool waits = Mode == suspension::queued || Mode == suspension::queued_void
                         || Mode == suspension::transferred;
  EXPECT_EQ(run.queue.submissions, waits ? 1 : 0);
  EXPECT_EQ(seen.attempted, Mode == suspension::ready ? 0 : 1);
  EXPECT_EQ(seen.transfers, Mode == suspension::transferred ? 1 : 0);
  if constexpr (waits) {
    EXPECT_EQ(seen.destroyed, 0);
    EXPECT_EQ(seen.resumed_address, nullptr);
    run.resume_cross_thread();
  }
  run.check_finished();
  run.consume_result();
  run.check_context(seen, waits ? 1 : 0);
  EXPECT_NE(seen.constructor_address, nullptr);
  EXPECT_EQ(seen.resumed_address, seen.constructor_address);
  EXPECT_EQ(seen.result, 42);
  EXPECT_EQ(seen.destroyed, 1);
  EXPECT_EQ(seen.after_expression, 1);
  EXPECT_EQ(run.queue.submissions, waits ? 1 : 0);
}

template <class Coroutine>
void check_named_and_converted() {
  std::array<observation, 5> inputs{};
  driver<Coroutine> borrowed;
  borrowed.start(wait_named<Coroutine>(inputs));
  borrowed.check_finished();
  borrowed.consume_result();
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto& seen = inputs[i];
    EXPECT_EQ(seen.result, 42) << i;
    EXPECT_EQ(seen.resumed_address, seen.constructor_address) << i;
    EXPECT_EQ(seen.destroyed, 1) << i;
    EXPECT_EQ(seen.after_expression, 0) << i;
    EXPECT_EQ(seen.factories, i >= 3 ? 1 : 0) << i;
  }

  std::array<observation, 2> converted{};
  driver<Coroutine> values;
  values.start(wait_factories<Coroutine>(converted));
  values.check_finished();
  values.consume_result();
  for (const auto& seen : converted) {
    EXPECT_EQ(seen.result, 42);
    EXPECT_EQ(seen.factories, 1);
    EXPECT_EQ(seen.unused_conversion, 0);
    EXPECT_EQ(seen.destroyed, 1);
    EXPECT_EQ(seen.after_expression, 1);
  }
}

template <class Coroutine>
void check_movable() {
  std::vector<move_event> events;
  events.reserve(6);
  driver<Coroutine> run;
  run.start(wait_movable<Coroutine>(events));
  run.check_finished();
  run.consume_result();
  EXPECT_EQ(events,
            (std::vector<move_event>{move_event::created,
                                     move_event::moved,
                                     move_event::resumed,
                                     move_event::destroy_owner,
                                     move_event::destroy_original,
                                     move_event::after_expression}));
}

TEST(CoroutineContractAwaiter, TaskTemporaryProtocols) {
  check_temporary<test_task, suspension::ready>();
  check_temporary<test_task, suspension::immediate>();
  check_temporary<test_task, suspension::queued>();
  check_temporary<test_task, suspension::queued_void>();
  check_temporary<test_task, suspension::transferred>();
}

TEST(CoroutineContractAwaiter, DetachedRootTemporaryProtocols) {
  check_temporary<root_task, suspension::ready>();
  check_temporary<root_task, suspension::immediate>();
  check_temporary<root_task, suspension::queued>();
  check_temporary<root_task, suspension::queued_void>();
  check_temporary<root_task, suspension::transferred>();
}

TEST(CoroutineContractAwaiter, NamedAndConvertedNativeAwaiters) {
  check_named_and_converted<test_task>();
  check_named_and_converted<root_task>();
}

TEST(CoroutineContractAwaiter, MovableTemporaryPreservesOwnershipAndDestructionOrder) {
  check_movable<test_task>();
  check_movable<root_task>();
}

TEST(CoroutineContractAwaiter, ThrowingResumeDestroysTemporaryAndPreservesContext) {
  observation ordinary;
  ordinary.throws = true;
  driver<test_task> task;
  task.start(wait_temporary<test_task, suspension::queued>(ordinary));
  EXPECT_EQ(ordinary.destroyed, 0);
  task.resume_cross_thread();
  task.check_finished();
  EXPECT_THROW(task.consume_result(), std::runtime_error);
  EXPECT_EQ(ordinary.destroyed, 1);
  task.check_context(ordinary, 1);

  observation root_seen;
  root_seen.throws = true;
  driver<root_task> root;
  root.start(wait_throwing_root(root_seen));
  EXPECT_EQ(root_seen.destroyed, 0);
  root.resume_cross_thread();
  root.check_finished();
  EXPECT_EQ(root_seen.destroyed, 1);
  EXPECT_EQ(root_seen.after_expression, 1);
  root.check_context(root_seen, 1);
}
}  // namespace
