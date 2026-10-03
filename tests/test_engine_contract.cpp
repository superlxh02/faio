/**
 * @file test_engine_contract.cpp
 * @brief 无 runtime、无协程的 owning IO engine 契约。
 * @details 中立完成目标直接记录结果，证明完成管线没有调度器依赖。
 */
#include "backend_test_support.hpp"
#include "faio/detail/io/engine.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <latch>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <sys/socket.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace {
struct descriptor_pair {
  std::array<int, 2> descriptors{-1, -1};

  descriptor_pair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()))
      throw std::system_error(errno, std::generic_category());
    for (const auto fd : descriptors) {
      if (::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK))
        throw std::system_error(errno, std::generic_category());
    }
  }

  descriptor_pair(const descriptor_pair&) = delete;

  descriptor_pair& operator=(const descriptor_pair&) = delete;

  ~descriptor_pair() {
    for (const auto fd : descriptors)
      if (fd >= 0)
        ::close(fd);
  }
};

/** @brief engine 的完成消费者；只在驱动/提交调用线程串行执行。 */
struct observed_completion {
  unsigned count{};
  std::int64_t result{};
  std::uint64_t transferred{};

  faio::io::detail::completion_target target() noexcept {
    return {this, [](void* owner, std::int64_t result, std::uint64_t progress) noexcept {
              auto& state = *static_cast<observed_completion*>(owner);
              ++state.count;
              state.result = result;
              state.transferred = progress;
            }};
  }
};

/** @brief 单事件 readiness 脚本；只注入真实方向位，不碰生产域的私有状态。 */
struct readiness_script {
  std::uint64_t key{};
  std::optional<faio::io::detail::readiness_event> event;
  std::atomic<unsigned> wakes{};  ///< 控制唤醒合同计数；不猜测 OS 是否已经进入等待。
};

std::shared_ptr<readiness_script> active_readiness_script;

/** @brief 最小 reactor 合同替身，使“没有写就绪”不依赖内核 socket 缓冲水位。 */
struct scripted_readiness {
  static constexpr const char* backend_name = "scripted-direction-readiness";
  std::shared_ptr<readiness_script> state;

  int attach(int, std::uint64_t key) noexcept {
    state->key = key;  // 后续事件必须使用已登记的资源代际。
    return 0;
  }

  void detach(int) noexcept { state->key = 0; }

  int poll(std::span<faio::io::detail::readiness_event> output, std::optional<int>) noexcept {
    if (output.empty() || !state->event)
      return 0;  // 从不等待，测试驱动次数自身就是上限。
    output.front() = *state->event;
    state->event.reset();  // 一次驱动只移交一次事件。
    return 1;
  }

  void wake() noexcept {
    state->wakes.fetch_add(1, std::memory_order_relaxed);  // 只统计真实 reactor 通知入口。
  }
};

faio::io::detail::reactor_box make_scripted_readiness() {
  return faio::io::detail::reactor_box{
      std::make_unique<scripted_readiness>(scripted_readiness{active_readiness_script})};
}

/** @brief 即使 ASSERT 提前退出也解开 provider 屏障，避免失败路径死锁。 */
struct latch_release_guard {
  std::latch& barrier;
  bool released{};

  void release() noexcept {
    if (!std::exchange(released, true))
      barrier.count_down();
  }

  ~latch_release_guard() { release(); }
};

faio::io::detail::io_request receive(faio::io::detail::resource_ptr resource, char& byte) {
  faio::io::detail::io_request request;
  request.resource = std::move(resource);
  request.kind = faio::io::detail::operation_kind::recv;
  request.buffer = &byte;
  request.length = 1;
  return request;
}
}  // namespace

/**
 * @brief 本地 SHUT_WR 后的读 Ready 必须等真正的 peer 数据，不能忙循环成功。
 * @details 直接用 engine 的中立 callback 和有限 drive 观察 pending；即使
 * 发生回归，也不会在当前测试线程运行无限的 async_io 重试循环。
 */
TEST(EngineContract, LocalWriteShutdownKeepsReadReadyPendingUntilPeerPayload) {
  descriptor_pair pair;
  faio::io::io_engine engine{faio_test::engine_config()};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(pair.descriptors[0], false);
  ASSERT_EQ(::shutdown(pair.descriptors[0], SHUT_WR), 0);
  faio::io::detail::io_request request;
  request.kind = faio::io::detail::operation_kind::ready;
  request.resource = resource;
  request.argument = faio::io::detail::readable_bit;
  observed_completion observed;
  int error{};
  const auto token = domain->prepare(std::move(request), observed.target(), error);
  ASSERT_NE(token.value, 0u);
  engine.submitter().submit(token);
  for (unsigned attempt = 0; attempt < 5; ++attempt)
    (void)engine.driver().wait_and_drive(10);  // 无数据观察窗口总计至多 50ms。
  EXPECT_EQ(observed.count, 0u) << "本地写关闭不能完成读方向 Ready";
  const char sent = 'r';
  ASSERT_EQ(::send(pair.descriptors[1], &sent, 1, 0), 1);
  for (unsigned attempt = 0; attempt < 5 && !observed.count; ++attempt)
    (void)engine.driver().wait_and_drive(10);
  ASSERT_EQ(observed.count, 1u);
  EXPECT_GT(observed.result & faio::io::detail::readable_bit, 0);
  char received{};
  ASSERT_EQ(::recv(pair.descriptors[0], &received, 1, MSG_DONTWAIT), 1);
  EXPECT_EQ(received, sent);
  for (unsigned attempt = 0; attempt < 3; ++attempt)
    (void)engine.driver().drive();  // 后续事件不能重复发布已终结的等待者。
  EXPECT_EQ(observed.count, 1u);
  EXPECT_TRUE(engine.driver().quiescent());
}

/** @brief peer 读 EOF 的 closed 提示不能替代缺失的写方向 readiness。 */
TEST(EngineContract, ReadEofAloneDoesNotCompleteWriteReady) {
  // 公开快照必须分别表达方向，不能让两个 getter 共用聚合 closed 位。
  const faio::io::Ready read_eof{faio::io::detail::readable_bit
                                 | faio::io::detail::read_closed_bit};
  EXPECT_TRUE(read_eof.is_read_closed());
  EXPECT_FALSE(read_eof.is_write_closed());
  const faio::io::Ready write_eof{faio::io::detail::writable_bit
                                  | faio::io::detail::write_closed_bit};
  EXPECT_FALSE(write_eof.is_read_closed());
  EXPECT_TRUE(write_eof.is_write_closed());
  const faio::io::Ready both_closed{faio::io::detail::closed_bit};
  EXPECT_TRUE(both_closed.is_read_closed());
  EXPECT_TRUE(both_closed.is_write_closed());
  descriptor_pair pair;
  active_readiness_script = std::make_shared<readiness_script>();
  auto config = faio_test::engine_config();
  config.reactor_factory = make_scripted_readiness;
  faio::io::io_engine engine{std::move(config)};
  auto resource = engine.context().domain()->adopt(pair.descriptors[0], false);
  faio::io::detail::io_request request;
  request.kind = faio::io::detail::operation_kind::ready;
  request.resource = resource;
  request.argument = faio::io::detail::writable_bit;
  observed_completion observed;
  int error{};
  const auto token =
      engine.context().domain()->prepare(std::move(request), observed.target(), error);
  ASSERT_NE(token.value, 0u);
  engine.submitter().submit(token);
  ASSERT_NE(active_readiness_script->key, 0u);
  active_readiness_script->event = faio::io::detail::readiness_event{
      active_readiness_script->key,
      faio::io::detail::readable_bit | faio::io::detail::read_closed_bit};
  for (unsigned attempt = 0; attempt < 3; ++attempt)
    (void)engine.driver().drive();  // 只有读 EOF，脚本明确没有发布可写位。
  EXPECT_EQ(observed.count, 0u);
  active_readiness_script->event = faio::io::detail::readiness_event{
      active_readiness_script->key, faio::io::detail::writable_bit};
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 1u);
  EXPECT_GT(observed.result & faio::io::detail::writable_bit, 0);
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 1u);
  EXPECT_TRUE(engine.driver().quiescent());
}

TEST(EngineContract, PreparedRequestHasNoIoSideEffectAndCapacityIsBounded) {
  descriptor_pair pair;
  auto config = faio_test::engine_config();
  config.max_operations = 1;
  faio::io::io_engine engine{std::move(config)};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(pair.descriptors[0], false);
  observed_completion first, rejected;
  char byte{};
  int error = 0;
  const auto token = domain->prepare(receive(resource, byte), first.target(), error);
  ASSERT_NE(token.value, 0u);
  EXPECT_EQ(first.count, 0u);
  const auto over_capacity = domain->prepare(receive(resource, byte), rejected.target(), error);
  EXPECT_EQ(over_capacity.value, 0u);
  EXPECT_EQ(error, EAGAIN);
  EXPECT_EQ(rejected.count, 0u);
  // Prepared 槽尚未 submit 也能被取消；接受后唯一完成承接这个 sticky 原因。
  engine.submitter().request_cancel(token);
  engine.submitter().submit(token);
  EXPECT_EQ(first.count, 1u);
  EXPECT_EQ(first.result, -ECANCELED);
  EXPECT_TRUE(engine.driver().quiescent());
}

TEST(EngineContract, OperationGenerationRejectsLateCancellationAfterSlotReuse) {
  descriptor_pair pair;
  auto config = faio_test::engine_config();
  config.max_operations = 1;
  faio::io::io_engine engine{std::move(config)};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(pair.descriptors[0], false);
  observed_completion old, fresh;
  char byte{};
  int error = 0;
  const auto retired = domain->prepare(receive(resource, byte), old.target(), error);
  engine.submitter().request_cancel(retired);
  engine.submitter().submit(retired);
  ASSERT_EQ(old.count, 1u);
  const auto token = domain->prepare(receive(resource, byte), fresh.target(), error);
  ASSERT_NE(token.value, 0u);
  ASSERT_NE(token.value, retired.value);
  engine.submitter().submit(token);
  engine.submitter().request_cancel(retired);
  const char sent = 'g';
  ASSERT_EQ(::send(pair.descriptors[1], &sent, 1, 0), 1);
  for (int i = 0; i < 5 && !fresh.count; ++i)
    engine.driver().wait_and_drive(100);
  EXPECT_EQ(fresh.count, 1u);
  EXPECT_EQ(fresh.result, 1);
  EXPECT_EQ(byte, sent);
  EXPECT_TRUE(engine.driver().quiescent());
}

/**
 * @brief 主操作池满时，RAII close 仍须提交原生 CLOSE 并等待真正 CQE。
 * @details 一个无数据 RECV 占满唯一业务槽；关闭独立资源不得取消该 RECV，
 * 不得偷走业务槽或转入辅助线程。reactor 同样验证唯一拥有权与容量边界。
 */
TEST(EngineContract, FullMainOperationPoolStillCompletesNativeOwnedResourceClose) {
  descriptor_pair pending_pair, closing_pair;
  auto config = faio_test::engine_config();
  config.max_operations = 1;
  faio::io::io_engine engine{config};
  auto domain = engine.context().domain();
  auto pending_resource = domain->adopt(pending_pair.descriptors[0], false);
  const int owned_descriptor = closing_pair.descriptors[0];
  auto closing_resource = domain->adopt(owned_descriptor, true);
  closing_pair.descriptors[0] = -1;  // fd 已转交唯一资源拥有者。
  observed_completion pending, rejected;
  char byte{};
  int error{};
  const auto token = domain->prepare(receive(pending_resource, byte), pending.target(), error);
  ASSERT_NE(token.value, 0u);
  engine.submitter().submit(token);
  ASSERT_EQ(pending.count, 0u);
  const auto before_close = engine.statistics();
  domain->request_close(closing_resource);  // 模拟最后一个 wrapper 析构。
  const bool native = domain->supports_native(faio::io::detail::operation_kind::close);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (native && std::chrono::steady_clock::now() < deadline
         && engine.statistics().native_completed <= before_close.native_completed)
    (void)engine.driver().wait_and_drive(10);
  const auto after_close = engine.statistics();
  EXPECT_EQ(closing_resource->fd(), -1);
  const int closed_status = ::fcntl(owned_descriptor, F_GETFD);
  const int closed_error = errno;
  EXPECT_EQ(closed_status, -1);
  EXPECT_EQ(closed_error, EBADF);
  EXPECT_EQ(pending.count, 0u);
  const auto full = domain->prepare(receive(pending_resource, byte), rejected.target(), error);
  EXPECT_EQ(full.value, 0u);
  EXPECT_EQ(error, EAGAIN);  // 内部 close 不得释放仍被 RECV 借用的业务槽。
  if (native) {
    EXPECT_GE(after_close.native_submitted, before_close.native_submitted + 1);
    EXPECT_GE(after_close.native_completed, before_close.native_completed + 1);
    EXPECT_GE(after_close.native_flushed, before_close.native_flushed + 1);
    EXPECT_EQ(engine.context().cleanup().started_threads(), 0u);
  }
  engine.submitter().request_cancel(token);
  const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while ((!pending.count || !engine.driver().quiescent())
         && std::chrono::steady_clock::now() < cancel_deadline)
    (void)engine.driver().wait_and_drive(10);
  EXPECT_EQ(pending.count, 1u);
  EXPECT_EQ(pending.result, -ECANCELED);
  EXPECT_TRUE(engine.driver().quiescent());
}

TEST(EngineContract, CancelAllShutdownCompletesPendingIoAndInvalidatesOwnedHandle) {
  descriptor_pair pair;
  faio::io::io_engine engine{faio_test::engine_config()};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(pair.descriptors[0], true);
  pair.descriptors[0] = -1;  // 完成显式拥有权转移，避免测试 RAII 二次 close。
  observed_completion result;
  char byte{};
  int error = 0;
  const auto token = domain->prepare(receive(resource, byte), result.target(), error);
  engine.submitter().submit(token);
  ASSERT_EQ(result.count, 0u);
  engine.begin_shutdown(faio::io::shutdown_policy::cancel_all);
  engine.shutdown();
  EXPECT_EQ(result.count, 1u);
  EXPECT_EQ(result.result, -ECANCELED);
  EXPECT_EQ(resource->fd(), -1);
  EXPECT_TRUE(engine.context().stopped());
  EXPECT_TRUE(engine.driver().quiescent());
}

TEST(EngineContract, ContextOutlivesEngineAndExternalResourceDestructionIsSafe) {
  descriptor_pair pair;
  faio::io::io_context context;
  faio::io::detail::resource_ptr resource;
  {
    faio::io::io_engine engine{faio_test::engine_config()};
    context = engine.context();
    resource = context.domain()->adopt(pair.descriptors[0], true);
    pair.descriptors[0] = -1;
    EXPECT_FALSE(context.stopped());
  }
  EXPECT_TRUE(context.stopped());
  EXPECT_EQ(resource->fd(), -1);
  resource.reset();
  context = {};
}

/** @brief 未 submit 的 Prepared
 * 槽仍归引擎所有，停机必须将它释放并唯一交付取消。 */
TEST(EngineContract, ShutdownReclaimsPreparedOperationWithoutSubmission) {
  descriptor_pair pair;
  auto config = faio_test::engine_config();
  config.max_operations = 1;
  faio::io::io_engine engine{std::move(config)};
  auto resource = engine.context().domain()->adopt(pair.descriptors[0], false);
  observed_completion completed;
  char byte{};
  int error = 0;
  const auto token =
      engine.context().domain()->prepare(receive(resource, byte), completed.target(), error);
  ASSERT_NE(token.value, 0u);
  engine.shutdown();
  EXPECT_TRUE(engine.driver().quiescent());
  EXPECT_EQ(completed.count, 1u);
  EXPECT_EQ(completed.result, -ECANCELED);
}

/** @brief generation 耗尽槽永久退休；其他槽继续服务，全部退休后仍正确判定排空。
 */
TEST(EngineContract, ExhaustedGenerationRetiresSlotWithoutBlockingOtherSlotsOrShutdown) {
  for (std::size_t capacity : {std::size_t{1}, std::size_t{2}}) {
    descriptor_pair pair;
    auto config = faio_test::engine_config();
    config.max_operations = capacity;
    faio::io::io_engine engine{std::move(config)};
    auto domain = engine.context().domain();
    auto resource = domain->adopt(pair.descriptors[0], false);
    observed_completion first, following;
    char byte{};
    int error = 0;
    const auto token = domain->prepare(receive(resource, byte), first.target(), error);
    ASSERT_NE(token.value, 0u);
    engine.submitter().submit(token);
    ASSERT_NE(resource->reader, nullptr);
    // 无并发 driver：通过稳定槽对象模拟 2^32 次使用，不引入生产测试 hook。
    resource->reader->generation = UINT32_MAX;
    engine.submitter().request_cancel(token);
    for (unsigned attempts = 0; attempts < 100 && !first.count; ++attempts)
      (void)engine.driver().wait_and_drive(10);
    ASSERT_EQ(first.count, 1u);
    const auto next = domain->prepare(receive(resource, byte), following.target(), error);
    if (capacity == 1) {
      EXPECT_EQ(next.value, 0u);
      EXPECT_EQ(error, EAGAIN);
    } else {
      ASSERT_NE(next.value, 0u);
      engine.submitter().request_cancel(next);
      engine.submitter().submit(next);
      (void)engine.driver().drive();
      EXPECT_EQ(following.count, 1u);
    }
    EXPECT_TRUE(engine.driver().quiescent());
    engine.shutdown();
    EXPECT_TRUE(engine.driver().quiescent());
  }
}

/** @brief 跨线程提交较早 deadline 必须唤醒已经在无限等待的 driver。 */
TEST(EngineContract, RemoteDeadlineSubmissionInterruptsParkedDriver) {
  descriptor_pair pair;
  faio::io::io_engine engine{faio_test::engine_config()};
  auto resource = engine.context().domain()->adopt(pair.descriptors[0], false);
  std::atomic<unsigned> count{0};
  std::atomic<std::int64_t> result{0};

  struct consumer {
    std::atomic<unsigned>& count;
    std::atomic<std::int64_t>& result;
  } observed{count, result};

  faio::io::detail::completion_target target{
      &observed, [](void* pointer, std::int64_t value, std::uint64_t) noexcept {
        auto& state = *static_cast<consumer*>(pointer);
        state.result.store(value, std::memory_order_relaxed);
        state.count.fetch_add(1, std::memory_order_release);
      }};
  char byte{};
  auto request = receive(resource, byte);
  std::jthread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{10};
    int error = 0;
    auto token = engine.context().domain()->prepare(std::move(request), target, error);
    if (token.value)
      engine.submitter().submit(token);
  });
  const auto start = std::chrono::steady_clock::now();
  for (int attempt = 0; attempt < 10 && !count.load(std::memory_order_acquire); ++attempt)
    engine.driver().wait_and_drive(200);
  EXPECT_EQ(count.load(std::memory_order_acquire), 1u);
  EXPECT_EQ(result.load(), -ETIMEDOUT);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds{150});
}

/** @brief 一个工作线程加一个队列槽形成可确定的满载，超限任务必须被拒绝。 */
TEST(BlockingExecutorContract, QueueCapacityAndCloseDrainAreDeterministic) {
  faio::execution::blocking_executor service{1, 1};
  std::latch started{1}, release{1};
  std::atomic<unsigned> completed{0};
  ASSERT_TRUE(service.try_submit([&] {
    started.count_down();
    release.wait();
    completed.fetch_add(1, std::memory_order_relaxed);
  }));
  started.wait();
  EXPECT_TRUE(service.try_submit([&] { completed.fetch_add(1, std::memory_order_relaxed); }));
  EXPECT_EQ(service.queued(), 1u);
  auto overflow = service.try_submit([&] { completed.fetch_add(100, std::memory_order_relaxed); });
  EXPECT_FALSE(overflow);
  if (!overflow) {
    EXPECT_EQ(overflow.error().value(), EAGAIN);
  }
  release.count_down();
  service.close();
  EXPECT_EQ(completed.load(), 2u);
  auto stopped = service.try_submit([] {});
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().value(), ECANCELED);
  service.close();
}

TEST(BlockingExecutorContract, InvalidConfigurationRejectsBeforeAnyWorkIsAccepted) {
  EXPECT_THROW((faio::execution::blocking_executor{0, 1}), std::invalid_argument);
  EXPECT_THROW((faio::execution::blocking_executor{1, 0}), std::invalid_argument);
  EXPECT_THROW((faio::execution::blocking_executor{1, 1, 2}), std::invalid_argument);
}

/** @brief 已取消的 queued 文件写不可产生副作用，后到的 close
 * 不能改写先获胜原因。 */
TEST(EngineContract, CancelledQueuedFileRequestSkipsSyscallAndPreservesFirstReason) {
  auto service = std::make_shared<faio::execution::blocking_executor>(1, 8);
  std::latch occupied{1}, release{1};
  latch_release_guard unblock{release};
  ASSERT_TRUE(service->try_submit([&] {
    occupied.count_down();
    release.wait();
  }));
  occupied.wait();
  auto config = faio_test::provider_engine_config();
  config.filesystem_service = service;
  faio::io::io_engine engine{std::move(config)};
  auto close_file = [](std::FILE* handle) {
    if (handle)
      std::fclose(handle);
  };
  std::unique_ptr<std::FILE, decltype(close_file)> file{std::tmpfile(), close_file};
  ASSERT_NE(file, nullptr);
  const int descriptor = ::fileno(file.get());
  auto resource = engine.context().domain()->adopt(descriptor, false, true);
  faio::io::detail::io_request request;
  request.resource = resource;
  request.kind = faio::io::detail::operation_kind::write;
  request.const_buffer = "cancelled";
  request.length = 9;
  request.offset = 0;
  observed_completion completed;
  int error = 0;
  const auto token =
      engine.context().domain()->prepare(std::move(request), completed.target(), error);
  ASSERT_NE(token.value, 0u);
  engine.submitter().submit(token);
  engine.submitter().request_cancel(token, faio::io::cancel_reason::deadline);
  engine.context().domain()->request_close(resource);
  (void)engine.driver().drive();
  EXPECT_EQ(completed.count,
            0u);  // provider 尚未排空，借用和请求状态必须继续存活。
  unblock.release();
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (!completed.count && std::chrono::steady_clock::now() < limit)
    (void)engine.driver().wait_and_drive(10);
  EXPECT_EQ(completed.count, 1u);
  EXPECT_EQ(completed.result, -ETIMEDOUT);
  struct stat attributes{};
  ASSERT_EQ(::fstat(descriptor, &attributes), 0);
  EXPECT_EQ(attributes.st_size, 0);
  engine.shutdown();
  service->close();
}

/** @brief 超过内部完成批大小的 ready/cancel 混合请求，每个目标仍只完成一次。
 * @details 128 个独立资源避免同方向冲突；较小事件预算要求多轮驱动全部交付。
 *          完成后重复取消旧 token 不得生成第二次交付，也不能阻止后续请求。
 */
TEST(EngineContract, BatchedReadyAndCancellationPublishEveryTargetExactlyOnce) {
  constexpr std::size_t count = 128;
  std::array<descriptor_pair, count> endpoints;
  auto config = faio_test::engine_config();
  config.max_operations = count + 1;
  config.native_queue_entries = 8;  // 小SQ显式触发原生排队/重试，操作池容量仍为129。
  faio::io::io_engine engine{std::move(config)};
  auto domain = engine.context().domain();
  std::array<faio::io::detail::resource_ptr, count> resources;
  std::array<faio::io::operation_token, count> tokens;
  std::array<observed_completion, count> completions;
  std::array<char, count> received{};
  for (std::size_t index = 0; index < count; ++index) {
    resources[index] = domain->adopt(endpoints[index].descriptors[0], false);
    int error = 0;
    tokens[index] = domain->prepare(
        receive(resources[index], received[index]), completions[index].target(), error);
    ASSERT_NE(tokens[index].value, 0u);
    engine.submitter().submit(tokens[index]);
    ASSERT_EQ(completions[index].count, 0u);
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (index % 3 == 0) {
      engine.submitter().request_cancel(tokens[index]);
    } else {
      const char byte = static_cast<char>('A' + index % 26);
      ASSERT_EQ(::send(endpoints[index].descriptors[1], &byte, 1, 0), 1);
    }
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (!engine.driver().quiescent() && std::chrono::steady_clock::now() < deadline)
    (void)engine.driver().wait_and_drive(10, faio::io::drive_budget{17, 19});
  ASSERT_TRUE(engine.driver().quiescent());
  for (std::size_t index = 0; index < count; ++index) {
    EXPECT_EQ(completions[index].count, 1u) << index;
    EXPECT_EQ(completions[index].result, index % 3 == 0 ? -ECANCELED : 1) << index;
    if (index % 3 != 0) {
      EXPECT_EQ(received[index], static_cast<char>('A' + index % 26));
    }
    engine.submitter().request_cancel(tokens[index]);
  }
  (void)engine.driver().drive();
  for (const auto& completion : completions)
    EXPECT_EQ(completion.count, 1u);
  observed_completion subsequent;
  char byte{};
  int error = 0;
  const auto next = domain->prepare(receive(resources[0], byte), subsequent.target(), error);
  ASSERT_NE(next.value, 0u);
  engine.submitter().request_cancel(next);
  engine.submitter().submit(next);
  EXPECT_EQ(subsequent.count, 1u);
  EXPECT_TRUE(engine.driver().quiescent());
}

/** @brief 首 send 已见 EAGAIN 而 stable
 * 池满时，组合帧退出必须释放此前取得的方向。 */
TEST(EngineContract, FusedReservationRollsBackWhenWouldBlockPreparationIsFull) {
  descriptor_pair pair;
  const int send_buffer = 4096;
  ASSERT_EQ(
      ::setsockopt(pair.descriptors[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)),
      0);
  const std::array<char, 16384> filling{};
  bool blocked{};
  for (unsigned attempt = 0; attempt < 128; ++attempt) {
    const auto result = ::send(pair.descriptors[0], filling.data(), filling.size(), 0);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      blocked = true;
      break;
    }
    ASSERT_TRUE(result > 0 || (result < 0 && errno == EINTR));
  }
  ASSERT_TRUE(blocked);  // 有界地构成真实内核背压，不以时钟或 mock syscall 判断。
  auto config = faio_test::engine_config();
  config.max_operations = 1;
  active_readiness_script = std::make_shared<readiness_script>();
  config.reactor_factory = make_scripted_readiness;  // 本测试专门验证 EAGAIN 的 readiness 交接。
  faio::io::io_engine engine{std::move(config)};
  auto domain = engine.context().domain();
  auto output = domain->adopt(pair.descriptors[0], false);
  auto input = domain->adopt(pair.descriptors[1], false);
  char byte{};
  observed_completion parked;
  int error{};
  const auto full = domain->prepare(receive(input, byte), parked.target(), error);
  ASSERT_NE(full.value,
            0u);  // Prepared 不执行 syscall，唯一稳定槽现在明确被占用。
  char owner{};
  {
    faio::io::detail::direction_lease lease{output, faio::io::Interest::writable, &owner};
    faio::io::detail::io_request first;
    first.kind = faio::io::detail::operation_kind::send;
    first.resource = output;
    first.const_buffer = &byte;
    first.length = 1;
    first.reservation = &owner;
    first.establish_reservation = true;
    const auto immediate = domain->try_immediate(first, false);
    EXPECT_FALSE(immediate);  // 首个真实 send 遇到 EAGAIN，并在同一锁内持有组合方向。
    observed_completion rejected;
    const auto failed = domain->prepare(std::move(first), rejected.target(), error);
    EXPECT_EQ(failed.value, 0u);
    EXPECT_EQ(error, EAGAIN);  // 拒绝发生在无内核借用的 prepare 边界。
    if (failed.value) {
      // 失败回归也排空意外取得的槽，不留存指向局部消费者的引用。
      engine.submitter().request_cancel(failed);
      engine.submitter().submit(failed);
    }
  }
  char later_owner{};
  const auto available = domain->reserve(*output, faio::io::Interest::writable, &later_owner);
  EXPECT_TRUE(available);  // 原组合返回之后方向再次可用，证明没有 gate 泄漏。
  domain->unreserve(*output, faio::io::Interest::writable, &later_owner);
  engine.submitter().request_cancel(full);
  engine.submitter().submit(full);
  EXPECT_EQ(parked.count, 1u);
  EXPECT_EQ(parked.result, -ECANCELED);
}

/** @brief 同一真实槽复用 UDP 地址/消息/scalar
 * 请求，验证原生内嵌指针与消息输出。 */
TEST(EngineContract, RecycledSlotPreservesRealDatagramAndMessagePayloadsAcrossKinds) {
  using namespace faio::io::detail;
  faio::io::unix::OwnedFd client{::socket(AF_INET, SOCK_DGRAM, 0)};
  faio::io::unix::OwnedFd server{::socket(AF_INET, SOCK_DGRAM, 0)};
  ASSERT_GE(client.get(), 0);
  ASSERT_GE(server.get(), 0);
  for (const auto fd : {client.get(), server.get()})
    ASSERT_EQ(::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK), 0);
  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::bind(server.get(), reinterpret_cast<sockaddr*>(&peer), sizeof(peer)), 0);
  socklen_t peer_length = sizeof(peer);
  ASSERT_EQ(::getsockname(server.get(), reinterpret_cast<sockaddr*>(&peer), &peer_length), 0);
  // 所有借用输入/输出早于 engine 创建，任何 ASSERT 失败都先排空内核再销毁参数。
  const std::array<char, 3> datagram{'u', 'd', 'p'};
  const std::array<char, 2> tail{'i', 'o'};
  std::array<char, 3> received{};
  std::array<char, 2> received_tail{};
  sockaddr_storage source{};
  socklen_t source_length = sizeof(source);
  std::array<iovec, 2> output{
      {{received.data(), received.size()}, {received_tail.data(), received_tail.size()}}};
  msghdr message{};
  observed_completion observed;  // engine 失败排空前始终保留 consumer。
  auto config = faio_test::engine_config();
  config.max_operations = 1;
  faio::io::io_engine engine{config};
  auto domain = engine.context().domain();
  auto sender = domain->adopt(client.get(), false);
  auto receiver = domain->adopt(server.get(), false);
  faio::io::operation_token previous;
  auto run = [&](io_request request, std::int64_t expected) {
    observed = {};
    int error{};
    const auto token = domain->prepare(std::move(request), observed.target(), error);
    EXPECT_NE(token.value, 0u);
    if (!token.value)
      return false;
    if (previous.value) {
      EXPECT_NE(token.value, previous.value);
      EXPECT_EQ(static_cast<std::uint32_t>(token.value),
                static_cast<std::uint32_t>(previous.value));  // 同一个固定槽，代际不断增加。
    }
    previous = token;
    engine.submitter().submit(token);
    for (unsigned attempt = 0; attempt < 20 && !observed.count; ++attempt)
      (void)engine.driver().wait_and_drive(10);  // 有界等待；失败仍由 engine 正常取消排空。
    EXPECT_EQ(observed.count, 1u);
    EXPECT_EQ(observed.result, expected);
    return observed.count == 1 && observed.result == expected;
  };

  io_request invalid;
  invalid.kind = operation_kind::send;
  invalid.resource = sender;
  invalid.deadline = std::chrono::steady_clock::time_point::min();
  ASSERT_TRUE(run(std::move(invalid),
                  -ETIMEDOUT));  // 下代无 timeout 不能继承旧 deadline。
  io_request sendto;
  sendto.kind = operation_kind::sendto;
  sendto.resource = sender;
  sendto.const_buffer = datagram.data();
  sendto.length = datagram.size();
  std::memcpy(&sendto.address, &peer, peer_length);
  sendto.address_length = peer_length;
  ASSERT_TRUE(run(std::move(sendto),
                  datagram.size()));  // source request 已移动，SQE 必须绑定稳定槽地址。
  io_request recvfrom;
  recvfrom.kind = operation_kind::recvfrom;
  recvfrom.resource = receiver;
  recvfrom.buffer = received.data();
  recvfrom.length = received.size();
  recvfrom.output_address = reinterpret_cast<sockaddr*>(&source);
  recvfrom.output_address_length = &source_length;
  ASSERT_TRUE(run(std::move(recvfrom), received.size()));
  EXPECT_EQ(received, datagram);
  EXPECT_EQ(source.ss_family, AF_INET);
  EXPECT_EQ(source_length, sizeof(sockaddr_in));

  // macOS 对已连接 UDP 的显式目的地址 sendto 返回 EISCONN；先验证未连接报文，
  // 再连接同一个 socket，继续用同一个固定槽验证 scalar 与聚集消息操作。
  io_request connect;
  connect.kind = operation_kind::connect;
  connect.resource = sender;
  std::memcpy(&connect.address, &peer, peer_length);
  connect.address_length = peer_length;
  ASSERT_TRUE(run(std::move(connect), 0));

  io_request scalar;
  scalar.kind = operation_kind::send;
  scalar.resource = sender;
  scalar.const_buffer = datagram.data();
  scalar.length = datagram.size();
  ASSERT_TRUE(run(std::move(scalar), datagram.size()));
  received.fill(0);
  io_request receive_scalar;
  receive_scalar.kind = operation_kind::recv;
  receive_scalar.resource = receiver;
  receive_scalar.buffer = received.data();
  receive_scalar.length = received.size();
  ASSERT_TRUE(run(std::move(receive_scalar), received.size()));
  EXPECT_EQ(received, datagram);

  io_request sendmsg;
  sendmsg.kind = operation_kind::sendmsg;
  sendmsg.resource = sender;
  sendmsg.vectors = {{const_cast<char*>(datagram.data()), datagram.size()},
                     {const_cast<char*>(tail.data()), tail.size()}};
  sendmsg.message.msg_iov = sendmsg.vectors.data();
  sendmsg.message.msg_iovlen = sendmsg.vectors.size();
  ASSERT_TRUE(run(std::move(sendmsg), datagram.size() + tail.size()));
  received.fill(0);
  message.msg_iov = output.data();
  message.msg_iovlen = output.size();
  message.msg_name = &source;
  message.msg_namelen = sizeof(source);
  io_request recvmsg;
  recvmsg.kind = operation_kind::recvmsg;
  recvmsg.resource = receiver;
  recvmsg.output_message = &message;
  ASSERT_TRUE(run(std::move(recvmsg), datagram.size() + tail.size()));
  EXPECT_EQ(received, datagram);
  EXPECT_EQ(received_tail, tail);
  EXPECT_EQ(message.msg_namelen, sizeof(sockaddr_in));
  EXPECT_EQ(message.msg_iov,
            output.data());  // native copyback 保持调用者的描述指针。
  EXPECT_EQ(message.msg_flags, 0);
  EXPECT_TRUE(engine.driver().quiescent());
}

/**
 * @brief 外域 typed Cancel 必须通知 owner 发布被取消的原请求。
 * @details 原请求先发生真实 EAGAIN；脚本 poll 永不等待且不注入就绪事件。
 *          Cancel 本身立即完成，但不能因本 token 已终态而漏掉其他完成的通知。
 */
TEST(EngineContract, ForeignTypedCancelWakesOwnerForOtherQueuedCompletion) {
  using namespace faio::io::detail;
  for (const bool fused : {false, true}) {
    SCOPED_TRACE(fused ? "prepare_submit" : "prepare then submit");
    descriptor_pair pair;
    observed_completion original,
        control;      // 先于 engine 构造，失败退出时仍覆盖 shutdown callback。
    char byte = '?';  // pending 原请求的缓冲区同样必须覆盖 engine 的排空生命周期。
    active_readiness_script = std::make_shared<readiness_script>();
    auto config = faio_test::engine_config();
    config.reactor_factory = make_scripted_readiness;  // 在 uring/epoll 矩阵中都明确验证 readiness
    // 契约。
    faio::io::io_engine engine{std::move(config)};
    auto domain = engine.context().domain();
    auto resource = domain->adopt(pair.descriptors[0], false);
    int error{};
    {
      faio::io::io_engine::binding owner{
          engine};  // 初始原请求由 owner 接受，不污染 foreign wake 计数。
      const auto token = domain->prepare_submit(receive(resource, byte), original.target(), error);
      ASSERT_NE(token.value, 0u);
      ASSERT_EQ(error, 0);
    }
    ASSERT_EQ(original.count, 0u);
    ASSERT_NE(active_readiness_script->key,
              0u);  // 真实 EAGAIN 已进入 readiness 注册状态。
    ASSERT_EQ(active_readiness_script->wakes.load(std::memory_order_relaxed), 0u);
    ASSERT_NE(current_domain,
              domain.get());  // foreign 是明确的线程内 owner 合同，不依赖调度时机。
    io_request cancel;
    cancel.resource = resource;
    cancel.kind = operation_kind::cancel;
    const auto token = fused ? domain->prepare_submit(std::move(cancel), control.target(), error)
                             : domain->prepare(std::move(cancel), control.target(), error);
    ASSERT_NE(token.value, 0u);
    ASSERT_EQ(error, 0);
    if (!fused)
      engine.submitter().submit(token);
    EXPECT_EQ(control.count, 1u);
    EXPECT_EQ(control.result, 0);
    EXPECT_EQ(original.count,
              0u);  // 提交线程仍只发布自己的 B，不能迁移 A 的 callback 责任。
    EXPECT_EQ(active_readiness_script->wakes.load(std::memory_order_relaxed), 1u);
    {
      faio::io::io_engine::binding owner{engine};
      (void)engine.driver().drive();  // 一次有限驱动即发布已终态
      // A，无事件、超时或重试掩盖缺通知。
      (void)engine.driver().drive();  // 第二次驱动证明没有重复发布。
    }
    EXPECT_EQ(original.count, 1u);
    EXPECT_EQ(original.result, -ECANCELED);
    EXPECT_EQ(original.transferred, 0u);
    EXPECT_EQ(byte, '?');  // 取消不消费借用缓冲区，也没有伪造成功 payload。
    EXPECT_EQ(control.count, 1u);
    EXPECT_TRUE(engine.driver().quiescent());
  }
}

/** @brief owner 本地接受 typed Cancel，完成链由自身下一次驱动发布，不新增冗余
 * wake。 */
TEST(EngineContract, LocalTypedCancelDoesNotWakeOwnerForOtherQueuedCompletion) {
  using namespace faio::io::detail;
  for (const bool fused : {false, true}) {
    SCOPED_TRACE(fused ? "prepare_submit" : "prepare then submit");
    descriptor_pair pair;
    observed_completion original, control;
    char byte = '?';
    active_readiness_script = std::make_shared<readiness_script>();
    auto config = faio_test::engine_config();
    config.reactor_factory = make_scripted_readiness;
    faio::io::io_engine engine{std::move(config)};
    auto domain = engine.context().domain();
    auto resource = domain->adopt(pair.descriptors[0], false);
    faio::io::io_engine::binding owner{engine};
    int error{};
    const auto original_token =
        domain->prepare_submit(receive(resource, byte), original.target(), error);
    ASSERT_NE(original_token.value, 0u);
    ASSERT_EQ(error, 0);
    ASSERT_EQ(original.count, 0u);
    ASSERT_NE(active_readiness_script->key, 0u);
    io_request cancel;
    cancel.resource = resource;
    cancel.kind = operation_kind::cancel;
    const auto token = fused ? domain->prepare_submit(std::move(cancel), control.target(), error)
                             : domain->prepare(std::move(cancel), control.target(), error);
    ASSERT_NE(token.value, 0u);
    ASSERT_EQ(error, 0);
    if (!fused)
      engine.submitter().submit(token);
    EXPECT_EQ(control.count, 1u);
    EXPECT_EQ(control.result, 0);
    EXPECT_EQ(original.count, 0u);
    EXPECT_EQ(active_readiness_script->wakes.load(std::memory_order_relaxed), 0u);
    (void)engine.driver().drive();
    (void)engine.driver().drive();
    EXPECT_EQ(original.count, 1u);
    EXPECT_EQ(original.result, -ECANCELED);
    EXPECT_EQ(byte, '?');
    EXPECT_EQ(control.count, 1u);
    EXPECT_TRUE(engine.driver().quiescent());
  }
}

/** @brief 普通外域立即成功的单个 recv 仍由 submit 发布，不能无理由通知 owner。
 */
TEST(EngineContract, ImmediateForeignReceiveDoesNotWakeForItsOwnTerminalOnly) {
  using namespace faio::io::detail;
  for (const bool fused : {false, true}) {
    SCOPED_TRACE(fused ? "prepare_submit" : "prepare then submit");
    descriptor_pair pair;
    observed_completion observed;
    char byte{};
    const char sent = 'w';
    ASSERT_EQ(::send(pair.descriptors[1], &sent, 1, 0),
              1);  // 明确制造真实即时 IO，不以验证错误代替成功分支。
    active_readiness_script = std::make_shared<readiness_script>();
    auto config = faio_test::engine_config();
    config.reactor_factory = make_scripted_readiness;
    faio::io::io_engine engine{std::move(config)};
    auto domain = engine.context().domain();
    auto resource = domain->adopt(pair.descriptors[0], false);
    ASSERT_NE(current_domain, domain.get());
    int error{};
    auto request = receive(resource, byte);
    const auto token = fused ? domain->prepare_submit(std::move(request), observed.target(), error)
                             : domain->prepare(std::move(request), observed.target(), error);
    ASSERT_NE(token.value, 0u);
    ASSERT_EQ(error, 0);
    if (!fused)
      engine.submitter().submit(token);
    EXPECT_EQ(observed.count, 1u);
    EXPECT_EQ(observed.result, 1);
    EXPECT_EQ(byte, sent);
    EXPECT_EQ(active_readiness_script->wakes.load(std::memory_order_relaxed), 0u);
    (void)engine.driver().drive();
    EXPECT_EQ(observed.count, 1u);
    EXPECT_TRUE(engine.driver().quiescent());
  }
}
