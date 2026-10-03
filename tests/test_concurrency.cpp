#include "backend_test_support.hpp"
#include <gtest/gtest.h>
#include "faio/faio.hpp"
#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

faio::task<void> scope_child_increment(std::atomic<int>& count) {
  co_await faio::this_coro::yield();
  count.fetch_add(1);
}
struct scope_body {
  std::atomic<int>& count;
  faio::task<int> operator()(faio::scope_context& scope) {
    scope.spawn(scope_child_increment(count));
    scope.spawn(scope_child_increment(count));
    co_return 13;
  }
};
struct ready_predicate {
  bool& value;
  bool operator()() const noexcept { return value; }
};

namespace {
faio::task<int> number(int value) { co_return value; }
faio::task<int> record_start(std::atomic<int>& started, int value) {
  started.fetch_add(1, std::memory_order_relaxed);
  co_return value;
}
faio::task<void> empty() { co_return; }
faio::task<int> joined() {
  auto [a, b, ignored] = co_await faio::join(number(2), number(3), empty());
  (void)ignored;
  co_return a + b;
}
faio::task<int> joined_all() {
  std::vector<faio::task<int>> tasks;
  for (int i = 0; i < 8; ++i) tasks.push_back(number(i));
  auto values = co_await faio::join_all(std::move(tasks));
  int sum = 0;
  for (int value : values) sum += value;
  co_return sum;
}
faio::task<std::size_t> selected() {
  auto winner = co_await faio::select(number(7), number(8));
  co_return winner.index;
}
faio::task<void> semaphore_worker(faio::sync::semaphore& sem,
                                  std::atomic<int>& active,
                                  std::atomic<int>& maximum) {
  co_await sem.acquire();
  auto n = active.fetch_add(1) + 1;
  auto old = maximum.load();
  while (old < n && !maximum.compare_exchange_weak(old, n)) {}
  co_await faio::time::sleep(std::chrono::milliseconds(1));
  active.fetch_sub(1);
  sem.release();
}
faio::task<void> barrier_worker(faio::sync::barrier& gate,
                                std::atomic<int>& arrived,
                                std::atomic<int>& passed) {
  arrived.fetch_add(1);
  co_await gate.arrive_and_wait();
  if (arrived.load() == 3) passed.fetch_add(1);
  co_await gate.arrive_and_wait();
}
faio::task<void> wait_broken_barrier(faio::sync::barrier& gate) {
  co_await gate.arrive_and_wait();
}
faio::task<void> latch_waiter(faio::sync::latch& done, std::atomic<bool>& flag) {
  co_await done.wait();
  flag.store(true);
}
faio::task<void> producer(faio::sync::mpsc<int>::sender sender) {
  for (int n = 1; n <= 64; ++n) {
    auto result = co_await sender.send(n);
    if (!result) throw std::runtime_error("send failed");
  }
}
faio::task<int> consumer(faio::sync::mpsc<int>::receiver& receiver) {
  int sum = 0;
  for (int i = 0; i < 128; ++i) {
    auto result = co_await receiver.recv();
    if (!result) throw std::runtime_error("recv failed");
    sum += *result;
  }
  co_return sum;
}
faio::task<int> mpsc_transfer() {
  auto endpoints = faio::sync::mpsc<int>::make(1);
  auto& send = endpoints.first;
  auto& recv = endpoints.second;
  auto [a, b, sum] = co_await faio::join(producer(send), producer(send), consumer(recv));
  (void)a; (void)b;
  co_return sum;
}
faio::task<void> delayed_spawn(std::atomic<int>& count) {
  co_await faio::time::sleep(std::chrono::milliseconds(1));
  faio::spawn_detached([](std::atomic<int>& x) -> faio::task<void> {
    co_await faio::time::sleep(std::chrono::milliseconds(1));
    x.fetch_add(1);
  }(count));
}
} // namespace

TEST(ConcurrencyTest, JoinAndJoinAll) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().set_num_workers(4).build()};
  std::atomic<int> started{0};
  auto combined = faio::join(record_start(started, 2), record_start(started, 3));
  EXPECT_EQ(started.load(std::memory_order_relaxed), 0);
  auto [left, right] = ctx.block_on(std::move(combined));
  EXPECT_EQ(left + right, 5);
  EXPECT_EQ(started.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(ctx.block_on(joined()), 5);
  EXPECT_EQ(ctx.block_on(joined_all()), 28);
  EXPECT_LT(ctx.block_on(selected()), 2u);
}
TEST(ConcurrencyTest, SemaphoreAndBarrier) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().set_num_workers(4).build()};
  faio::sync::semaphore sem{2};
  std::atomic<int> active{0}, maximum{0};
  ctx.block_on(faio::join(semaphore_worker(sem, active, maximum),
                                 semaphore_worker(sem, active, maximum),
                                 semaphore_worker(sem, active, maximum),
                                 semaphore_worker(sem, active, maximum)));
  EXPECT_LE(maximum.load(), 2);
  EXPECT_EQ(active.load(), 0);
  faio::sync::barrier gate{3};
  std::atomic<int> arrived{0}, passed{0};
  ctx.block_on(faio::join(barrier_worker(gate, arrived, passed),
                                 barrier_worker(gate, arrived, passed),
                                 barrier_worker(gate, arrived, passed)));
  EXPECT_EQ(passed.load(), 3);
}
TEST(ConcurrencyTest, LatchCrossThreadNotification) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().set_num_workers(2).build()};
  faio::sync::latch done{1};
  std::atomic<bool> flag{false};
  std::thread notifier([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    done.count_down();
  });
  ctx.block_on(latch_waiter(done, flag));
  notifier.join();
  EXPECT_TRUE(flag.load());
}
TEST(ConcurrencyTest, BoundedMpscAndTrackedSpawn) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().set_num_workers(4).build()};
  EXPECT_EQ(ctx.block_on(mpsc_transfer()), 4160);
  std::atomic<int> count{0};
  ctx.block_on(delayed_spawn(count));
  EXPECT_EQ(count.load(), 1);
}

TEST(ConcurrencyTest, ExternalSpawnIsDrainedOnStop) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().set_num_workers(2).build()};
  std::atomic<int> count{0};
  ctx.submit(delayed_spawn(count));
  ctx.stop(faio::io::shutdown_policy::drain);
  EXPECT_EQ(count.load(), 1);
}

namespace {
faio::task<void> stress_producer(faio::sync::mpsc<int>::sender sender,
                                 int producer_id, int count) {
  for (int i = 0; i < count; ++i) {
    auto result = co_await sender.send(producer_id * count + i);
    if (!result) throw std::runtime_error("stress send closed");
  }
}
faio::task<long long> stress_consumer(faio::sync::mpsc<int>::receiver receiver,
                                      int total) {
  long long sum = 0;
  for (int i = 0; i < total; ++i) {
    auto result = co_await receiver.recv();
    if (!result) throw std::runtime_error("stress recv closed");
    sum += *result;
  }
  co_return sum;
}
faio::task<long long> stress_mpsc(std::size_t capacity) {
  auto endpoints = faio::sync::mpsc<int>::make(capacity);
  constexpr int producers = 4, per_producer = 1000;
  for (int i = 0; i < producers; ++i)
    faio::spawn_detached(stress_producer(endpoints.first, i, per_producer));
  co_return co_await stress_consumer(std::move(endpoints.second),
                                     producers * per_producer);
}
faio::task<int> lease_survives_sender_close() {
  auto endpoints = faio::sync::mpsc<int>::make(2);
  auto pending = endpoints.first.send(41); // 尚未运行的发送任务也持有发送者租约
  endpoints.first.close();
  auto result = co_await std::move(pending);
  if (!result) co_return -1;
  auto received = co_await endpoints.second.recv();
  co_return received ? *received : -2;
}
faio::task<bool> closed_receiver_rejects_send() {
  auto endpoints = faio::sync::mpsc<int>::make(2);
  endpoints.second.close();
  auto result = co_await endpoints.first.send(1);
  co_return !result && result.error().value() == faio::Error::ClosedChannel;
}
faio::task<void> close_receiver_after(faio::sync::mpsc<int>::receiver& receiver) {
  co_await faio::time::sleep(std::chrono::milliseconds(1));
  receiver.close();
}
faio::task<bool> pending_recv_observes_close() {
  auto endpoints = faio::sync::mpsc<int>::make(2);
  faio::spawn_detached(close_receiver_after(endpoints.second));
  auto result = co_await endpoints.second.recv();
  co_return !result && result.error().value() == faio::Error::ClosedChannel;
}
faio::task<int> move_only_channel_value() {
  auto endpoints = faio::sync::mpsc<std::unique_ptr<int>>::make(2);
  auto sent = co_await endpoints.first.send(std::make_unique<int>(17));
  if (!sent) co_return -1;
  auto received = co_await endpoints.second.recv();
  co_return received ? **received : -2;
}
faio::task<std::unique_ptr<int>> move_only_value() {
  co_return std::make_unique<int>(33);
}
faio::task<int> deep_chain(int depth) {
  if (depth == 0) co_return 0;
  co_return 1 + co_await deep_chain(depth - 1);
}
faio::task<int> failing_task() {
  throw std::runtime_error("join error");
  co_return 0;
}
faio::task<int> slow_loser(std::atomic<bool>& finished) {
  co_await faio::time::sleep(std::chrono::milliseconds(4));
  finished.store(true);
  co_return 2;
}
faio::task<int> cancelable_sleep_loser() {
  co_await faio::time::sleep(std::chrono::seconds(1));
  co_return 2;
}
faio::task<std::size_t> select_cancelable_sleep() {
  auto result = co_await faio::select(number(1), cancelable_sleep_loser());
  co_return result.index;
}
faio::task<void> select_with_loser(std::atomic<bool>& finished) {
  auto result = co_await faio::select(number(1), slow_loser(finished));
  if (result.index > 1) throw std::runtime_error("invalid select index");
}

faio::task<int> inspect_coro_context() {
  auto scheduler = co_await faio::this_coro::scheduler();
  auto worker = co_await faio::this_coro::worker_id();
  auto priority = co_await faio::this_coro::priority();
  auto token = co_await faio::this_coro::stop_token();
  co_await faio::this_coro::yield();
  co_await faio::this_coro::yield_if_needed();
  co_return scheduler && worker != std::numeric_limits<std::size_t>::max() &&
            priority == faio::task_priority::normal && !token.stop_requested();
}
faio::task<faio::task_priority> inspect_priority() {
  co_return co_await faio::this_coro::priority();
}
faio::task<int> scope_run(std::atomic<int>& count) {
  co_return co_await faio::scope(scope_body{count});
}
faio::task<int> join_handle_run() {
  auto handle = faio::spawn(number(42));
  co_return co_await handle;
}
faio::task<bool> token_requested() {
  auto token = co_await faio::this_coro::stop_token();
  co_return token.stop_requested();
}
faio::task<bool> yield_then_spawn(std::atomic<bool>& start) {
  while (!start.load(std::memory_order_acquire))
    co_await faio::this_coro::yield();
  co_await faio::this_coro::yield();
  auto child = faio::spawn(token_requested());
  co_return co_await child;
}
faio::task<int> sem_wait_forever(faio::sync::semaphore& sem) {
  co_await sem.acquire();
  co_return 3;
}
faio::task<int> recv_wait_forever(faio::sync::mpsc<int>::receiver& receiver) {
  auto value = co_await receiver.recv();
  co_return value ? *value : -1;
}
faio::task<int> barrier_wait_forever(faio::sync::barrier& gate) {
  co_await gate.arrive_and_wait();
  co_return 3;
}
faio::task<int> select_sem_cancel(faio::sync::semaphore& sem) {
  auto result = co_await faio::select(number(1), sem_wait_forever(sem));
  co_return static_cast<int>(result.index);
}
faio::task<int> select_recv_cancel(faio::sync::mpsc<int>::receiver& receiver) {
  auto result = co_await faio::select(number(1), recv_wait_forever(receiver));
  co_return static_cast<int>(result.index);
}
faio::task<int> select_barrier_cancel(faio::sync::barrier& gate) {
  auto result = co_await faio::select(number(1), barrier_wait_forever(gate));
  co_return static_cast<int>(result.index);
}
faio::task<int> cv_wait_forever(faio::sync::condition_variable& cv,
                                faio::sync::mutex& mtx, bool& ready) {
  co_await mtx.lock();
  try {
    co_await cv.wait(mtx, ready_predicate{ready});
    mtx.unlock();
    co_return 1;
  } catch (...) {
    mtx.unlock();
    throw;
  }
}
faio::task<int> select_cv_cancel(faio::sync::condition_variable& cv,
                                 faio::sync::mutex& mtx, bool& ready) {
  auto result = co_await faio::select(number(1), cv_wait_forever(cv, mtx, ready));
  co_return static_cast<int>(result.index);
}
faio::task<int> recv_socket_wait(int fd) {
  char byte{};
  auto result = co_await faio::io::recv(fd, &byte, 1, 0);
  co_return result ? static_cast<int>(*result) : -1;
}
faio::task<int> select_socket_cancel(int fd) {
  auto result = co_await faio::select(number(1), recv_socket_wait(fd));
  co_return static_cast<int>(result.index);
}
faio::task<void> long_sleep() {
  co_await faio::time::sleep(std::chrono::seconds(1));
}
} // namespace

struct failed_scope_body {
  faio::task<void> operator()(faio::scope_context& scope) {
    scope.spawn(long_sleep());
    scope.spawn(failing_task());
    co_return;
  }
};
faio::task<void> failed_scope() {
  co_await faio::scope(failed_scope_body{});
}

TEST(ConcurrencyTest, MpscContentionAndClosure) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().set_num_workers(4).build()};
  constexpr long long expected = 3999LL * 4000 / 2;
  for (auto capacity : {1u, 2u, 3u, 64u})
    EXPECT_EQ(ctx.block_on(stress_mpsc(capacity)), expected);
  EXPECT_EQ(ctx.block_on(lease_survives_sender_close()), 41);
  EXPECT_TRUE(ctx.block_on(closed_receiver_rejects_send()));
  EXPECT_TRUE(ctx.block_on(pending_recv_observes_close()));
  EXPECT_EQ(ctx.block_on(move_only_channel_value()), 17);
}
TEST(ConcurrencyTest, TaskOwnershipAndSelectDrain) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().set_num_workers(4).build()};
  auto pointer = ctx.block_on(move_only_value());
  ASSERT_TRUE(pointer);
  EXPECT_EQ(*pointer, 33);
  EXPECT_EQ(ctx.block_on(deep_chain(1000)), 1000);
  EXPECT_THROW(ctx.block_on(faio::join(number(1), failing_task())),
               std::runtime_error);
  std::atomic<bool> finished{false};
  ctx.block_on(select_with_loser(finished));
  // select 请求停止落选分支，并等待其退出后返回。
  EXPECT_FALSE(finished.load());
  auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(ctx.block_on(select_cancelable_sleep()), 0u);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(100));
  ctx.stop();
}

TEST(ConcurrencyTest, JoinHandleScopeAndThisCoro) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().set_num_workers(2).build()};
  EXPECT_EQ(ctx.block_on(join_handle_run()), 42);
  auto external = ctx.spawn_observed(number(12));
  EXPECT_EQ(external.get(), 12);
  std::atomic<int> count{0};
  EXPECT_EQ(ctx.block_on(scope_run(count)), 13);
  EXPECT_EQ(count.load(), 2);
  EXPECT_EQ(ctx.block_on(inspect_coro_context()), 1);
  EXPECT_EQ(ctx.block_on(inspect_priority().with_priority(faio::task_priority::high)),
            faio::task_priority::high);
  std::atomic<bool> start{false};
  auto parent = ctx.spawn_observed(yield_then_spawn(start));
  parent.request_stop();
  start.store(true, std::memory_order_release);
  EXPECT_TRUE(parent.get());
}

TEST(ConcurrencyTest, SelectCancelsPrimitiveWaitersAndScopeDrains) {
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().set_num_workers(4).build()};
  faio::sync::semaphore sem{0};
  EXPECT_EQ(ctx.block_on(select_sem_cancel(sem)), 0);
  sem.release();
  EXPECT_TRUE(sem.try_acquire());
  auto endpoints = faio::sync::mpsc<int>::make(1);
  EXPECT_EQ(ctx.block_on(select_recv_cancel(endpoints.second)), 0);
  auto sent = endpoints.first.try_send(4);
  ASSERT_TRUE(sent);
  EXPECT_TRUE(*sent);
  auto value = endpoints.second.try_recv();
  ASSERT_TRUE(value);
  ASSERT_TRUE(value->has_value());
  EXPECT_EQ(**value, 4);
  faio::sync::barrier gate{2};
  EXPECT_EQ(ctx.block_on(select_barrier_cancel(gate)), 0);
  EXPECT_THROW(ctx.block_on(wait_broken_barrier(gate)), faio::operation_cancelled);
  faio::sync::condition_variable cv;
  faio::sync::mutex mtx;
  bool ready = false;
  EXPECT_EQ(ctx.block_on(select_cv_cancel(cv, mtx, ready)), 0);
  EXPECT_TRUE(mtx.try_lock());
  mtx.unlock();
  const auto start = std::chrono::steady_clock::now();
  EXPECT_THROW(ctx.block_on(failed_scope()), std::runtime_error);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
  auto canceled = ctx.spawn_observed(long_sleep());
  canceled.request_stop();
  EXPECT_THROW(canceled.get(), faio::operation_cancelled);
}

TEST(ConcurrencyTest, SelectCancelsIoUringWaiter) {
  int sockets[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  faio::runtime::detail::runtime_context ctx{faio_test::config_builder().set_num_workers(2).build()};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(ctx.block_on(select_socket_cancel(sockets[0])), 0);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
  ::close(sockets[0]);
  ::close(sockets[1]);
}
