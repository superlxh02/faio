/**
 * @file test_native_completion_contract.cpp
 * @brief 原生完成协议的确定性契约：取消 ACK、最终结果与 buffer release 分离。
 * @details 使用公开 backend_factory 注入一个可逐事件推进的 Proactor；不会依赖
 *          真内核偶然产生某种 CQE 顺序，也不会在测试中把 ACK 当作请求终止。
 */
#include "backend_test_support.hpp"
#include <array>
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <memory>
#include <stop_token>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace {
using namespace faio::io;
using namespace faio::io::detail;

struct completion {
  unsigned count{};
  std::int64_t result{};
  std::uint64_t progress{};
  completion_target target() noexcept {
    return {this, +[](void *pointer, std::int64_t result,
                      std::uint64_t progress) noexcept {
              auto &state = *static_cast<completion *>(pointer);
              ++state.count;
              state.result = result;
              state.progress = progress;
            }};
  }
};

/** @brief 测试自身拥有脚本，不修改生产引擎中的私有状态或代际。 */
struct script {
  std::unordered_map<std::uint64_t, io_request *> accepted;
  std::deque<backend_event> events;
  std::unordered_set<std::uint64_t> waiting_release;
  backend_statistics counters;
  bool allow_submit{true};
  bool auto_terminal_on_shutdown{true};
  bool failure_started{};
  int flush_error{};
};
std::shared_ptr<script> active_script;

/** @brief 接受之后始终保留 request 引用，终止 CQE/释放通知才解除它。 */
struct scripted_proactor {
  static constexpr bool native_proactor = true;
  std::shared_ptr<script> state;
  const char *name() const noexcept { return "scripted-proactor"; }
  int attach(int, std::uint64_t) noexcept { return 0; }
  void detach(int) noexcept {}
  bool supports(std::uint32_t kind) const noexcept {
    // Cancel是domain稳定资源控制协议，不是借用request的native原操作；真实backend同样不提交它。
    return kind != static_cast<std::uint32_t>(operation_kind::cancel);
  }
  backend_submit_result try_submit(backend_operation operation) noexcept {
    if (!state->allow_submit)
      return {backend_submit_status::would_queue, 0};
    state->accepted.emplace(operation.token,
                            static_cast<io_request *>(operation.request));
    ++state->counters.native_submitted;
    return {backend_submit_status::accepted, 0};
  }
  backend_flush_result flush() noexcept {
    return {0, false, state->flush_error};
  }
  void request_cancel(std::uint64_t token) noexcept {
    if (state->accepted.contains(token))
      state->events.push_back({backend_event_kind::cancel_ack, token, 0, 0});
  }
  int poll(std::span<backend_event> output, std::optional<int>) noexcept {
    std::size_t count{};
    while (count < output.size() && !state->events.empty()) {
      auto event = state->events.front();
      state->events.pop_front();
      output[count++] = event;
      if (event.kind == backend_event_kind::cancel_ack) {
        ++state->counters.cancel_ack;
      } else if (event.kind == backend_event_kind::buffer_release) {
        ++state->counters.buffer_notifications;
        state->waiting_release.erase(event.key);
        state->accepted.erase(event.key);
      } else if (event.kind == backend_event_kind::result) {
        ++state->counters.native_completed;
        if (event.flags & 1u)
          state->waiting_release.insert(event.key);
        else
          state->accepted.erase(event.key);
      }
    }
    return static_cast<int>(count);
  }
  void wake() const noexcept {}
  void begin_shutdown() noexcept {
    if (!state->auto_terminal_on_shutdown)
      return;
    // ASSERT 提前离开测试也能安全排空 mock，避免失败路径卡在 engine 析构。
    for (const auto &[token, request] : state->accepted) {
      (void)request;
      state->events.push_back(
          state->waiting_release.contains(token)
              ? backend_event{backend_event_kind::buffer_release, token, 0, 0}
              : backend_event{backend_event_kind::result, token, -ECANCELED,
                              0});
    }
  }
  bool begin_failure(int) noexcept {
    state->failure_started = true;
    return true; // mock仍能交付原请求的最终事件，证明其引用可安全排空。
  }
  bool quiescent() const noexcept {
    return state->accepted.empty() && state->events.empty();
  }
  backend_statistics statistics() const noexcept { return state->counters; }
};

backend_box make_scripted_backend() {
  return backend_box{
      std::make_unique<scripted_proactor>(scripted_proactor{active_script})};
}

struct socket_pair {
  std::array<int, 2> fd{-1, -1};
  socket_pair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fd.data()))
      throw std::system_error(errno, std::generic_category());
    for (const auto descriptor : fd)
      if (::fcntl(descriptor, F_SETFL, O_NONBLOCK))
        throw std::system_error(errno, std::generic_category());
  }
  ~socket_pair() {
    for (const auto descriptor : fd)
      if (descriptor >= 0)
        ::close(descriptor);
  }
};

engine_config scripted_config() {
  active_script = std::make_shared<script>();
  auto config = faio_test::engine_config();
  config.backend_factory = make_scripted_backend;
  config.max_operations = 1;
  return config;
}
} // namespace

TEST(NativeCompletionContract,
     CancellationAcknowledgementKeepsBorrowAndSlotUntilOriginalResult) {
  socket_pair endpoints;
  io_engine engine{scripted_config()};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(endpoints.fd[0], false);
  char borrowed{};
  io_request request;
  request.kind = operation_kind::recv;
  request.resource = resource;
  request.buffer = &borrowed;
  request.length = 1;
  completion observed;
  int error{};
  const auto token =
      domain->prepare_submit(std::move(request), observed.target(), error);
  ASSERT_NE(token.value, 0u);
  engine.submitter().submit(token); // 旧 API 再提交同 token 仍无副作用。
  EXPECT_EQ(active_script->counters.native_submitted, 1u);
  ASSERT_TRUE(active_script->accepted.contains(token.value));
  auto *stable_request = active_script->accepted.at(token.value);
  engine.submitter().request_cancel(token);
  (void)engine.driver().drive();
  EXPECT_EQ(active_script->counters.cancel_ack, 1u);
  EXPECT_EQ(observed.count, 0u);
  EXPECT_FALSE(engine.driver().quiescent());
  ASSERT_EQ(active_script->accepted.at(token.value), stable_request);
  EXPECT_EQ(stable_request->buffer, &borrowed);
  *static_cast<char *>(stable_request->buffer) =
      'k'; // 模拟 ACK 后仍合法的内核 buffer 引用。
  EXPECT_EQ(borrowed, 'k');
  io_request rejected;
  rejected.kind = operation_kind::recv;
  rejected.resource = resource;
  rejected.buffer = &borrowed;
  rejected.length = 1;
  completion unused;
  EXPECT_EQ(domain->prepare(std::move(rejected), unused.target(), error).value,
            0u);
  EXPECT_EQ(error, EAGAIN);
  active_script->events.push_back(
      {backend_event_kind::result, token.value, -ECANCELED, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 1u);
  EXPECT_EQ(observed.result, -ECANCELED);
  EXPECT_TRUE(engine.driver().quiescent());
  engine.submitter().request_cancel(token);
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 1u);
}

/** @brief File外层lease借用的无resource原生fd同样接受Cancel(fd,ALL|FD)。 */
TEST(NativeCompletionContract,
     BorrowedNativeFileDescriptorCancellationKeepsBufferUntilOriginalTerminal) {
  socket_pair endpoints;
  auto config = scripted_config();
  config.max_operations = 2; // 一个原File请求、一个短控制请求同时存在。
  io_engine engine{config};
  auto domain = engine.context().domain();
  char borrowed{};
  io_request request;
  request.kind = operation_kind::read;
  request.bypass_resource_registration = true;
  request.fd = endpoints.fd[0];
  request.buffer = &borrowed;
  request.length = 1;
  request.offset = 0;
  completion original;
  int error{};
  const auto token =
      domain->prepare(std::move(request), original.target(), error);
  ASSERT_NE(token.value, 0u);
  engine.submitter().submit(token);
  ASSERT_TRUE(active_script->accepted.contains(token.value));
  auto *stable_request = active_script->accepted.at(token.value);
  ASSERT_FALSE(stable_request->resource);
  io_request control_request;
  control_request.kind = operation_kind::cancel;
  control_request.fd = endpoints.fd[0];
  control_request.flags = 3; // ALL|FD：控制完成不能归还原File的借用。
  completion control;
  const auto control_token =
      domain->prepare(std::move(control_request), control.target(), error);
  ASSERT_NE(control_token.value, 0u);
  engine.submitter().submit(control_token);
  (void)engine.driver().drive();
  EXPECT_EQ(control.count, 1u);
  EXPECT_EQ(control.result, 0);
  EXPECT_EQ(active_script->counters.cancel_ack, 1u);
  EXPECT_EQ(original.count, 0u);
  ASSERT_TRUE(active_script->accepted.contains(token.value));
  ASSERT_EQ(active_script->accepted.at(token.value), stable_request);
  *static_cast<char *>(stable_request->buffer) = 'f';
  EXPECT_EQ(borrowed, 'f');
  EXPECT_FALSE(engine.driver().quiescent());
  active_script->events.push_back(
      {backend_event_kind::result, token.value, -ECANCELED, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(original.count, 1u);
  EXPECT_EQ(original.result, -ECANCELED);
  EXPECT_TRUE(engine.driver().quiescent());
}

/** @brief
 * 两个共享Close只使用一个原生CQE，secondary取消不能把waiter链留在复用槽上。 */
TEST(NativeCompletionContract,
     SharedCloseWaitersIgnoreCancellationUntilOneNativeCloseCompletes) {
  socket_pair endpoints;
  auto config = scripted_config();
  config.max_operations = 2;
  io_engine engine{config};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(endpoints.fd[0], true);
  completion first, second;
  int error{};
  auto first_request = make_request(resource, operation_kind::close);
  const auto first_token =
      domain->prepare(std::move(first_request), first.target(), error);
  ASSERT_NE(first_token.value, 0u);
  engine.submitter().submit(first_token);
  ASSERT_EQ(active_script->accepted.size(), 1u);
  auto second_request = make_request(resource, operation_kind::close);
  second_request.deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{1};
  const auto second_token =
      domain->prepare(std::move(second_request), second.target(), error);
  ASSERT_NE(second_token.value, 0u);
  engine.submitter().submit(second_token);
  domain->request_cancel(second_token, cancel_reason::user);
  domain->request_cancel(second_token, cancel_reason::deadline);
  std::this_thread::sleep_for(std::chrono::milliseconds{2});
  (void)engine.driver().drive();
  EXPECT_EQ(first.count, 0u);
  EXPECT_EQ(second.count, 0u);
  EXPECT_EQ(active_script->accepted.size(), 1u);
  EXPECT_EQ(active_script->counters.native_submitted, 1u);
  EXPECT_EQ(active_script->counters.cancel_ack, 0u);
  completion unused;
  auto peer_resource = domain->adopt(endpoints.fd[1], false);
  char unused_buffer{};
  auto extra = make_request(peer_resource, operation_kind::recv);
  extra.buffer = &unused_buffer;
  extra.length = 1;
  EXPECT_EQ(domain->prepare(std::move(extra), unused.target(), error).value,
            0u);
  EXPECT_EQ(error, EAGAIN); // secondary尚未终止，普通业务槽不能回收/复用。
  // 脚本模拟内核执行CLOSE后产生唯一CQE；失败退出时socket_pair仍负责实际fd兜底。
  ASSERT_EQ(::close(endpoints.fd[0]), 0);
  endpoints.fd[0] = -1;
  active_script->events.push_back(
      {backend_event_kind::result, first_token.value, 0, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(first.count, 1u);
  EXPECT_EQ(second.count, 1u);
  EXPECT_EQ(first.result, 0);
  EXPECT_EQ(second.result, 0);
  EXPECT_TRUE(engine.driver().quiescent());
  completion repeated;
  auto repeated_request = make_request(resource, operation_kind::close);
  const auto repeated_token =
      domain->prepare(std::move(repeated_request), repeated.target(), error);
  ASSERT_NE(repeated_token.value, 0u);
  engine.submitter().submit(repeated_token);
  (void)engine.driver().drive();
  EXPECT_EQ(repeated.count, 1u);
  EXPECT_EQ(repeated.result, 0);
  EXPECT_EQ(active_script->counters.native_submitted, 1u);
  EXPECT_TRUE(engine.driver().quiescent());
}

TEST(NativeCompletionContract,
     ZeroCopyResultWaitsForBufferReleaseBeforeCompletionAndReuse) {
  socket_pair endpoints;
  io_engine engine{scripted_config()};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(endpoints.fd[0], false);
  const std::array<char, 5> borrowed{'o', 'w', 'n', 'e', 'd'};
  io_request request;
  request.kind = operation_kind::send_zc;
  request.resource = resource;
  request.const_buffer = borrowed.data();
  request.length = borrowed.size();
  completion observed;
  int error{};
  const auto token =
      domain->prepare(std::move(request), observed.target(), error);
  ASSERT_NE(token.value, 0u);
  engine.submitter().submit(token);
  active_script->events.push_back(
      {backend_event_kind::result, token.value, 5, 1});
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 0u);
  EXPECT_FALSE(engine.driver().quiescent());
  ASSERT_TRUE(active_script->accepted.contains(token.value));
  EXPECT_EQ(active_script->accepted.at(token.value)->const_buffer,
            borrowed.data());
  active_script->events.push_back(
      {backend_event_kind::buffer_release, token.value, 0, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 1u);
  EXPECT_EQ(observed.result, 5);
  EXPECT_EQ(observed.progress, 5u);
  EXPECT_EQ(active_script->counters.native_completed, 1u);
  EXPECT_EQ(active_script->counters.buffer_notifications, 1u);
  EXPECT_TRUE(engine.driver().quiescent());
  // 迟到的重复通知只会查到已经无效的旧代际，不能恢复完成消费者两次。
  active_script->events.push_back(
      {backend_event_kind::buffer_release, token.value, 0, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 1u);
}

TEST(
    NativeCompletionContract,
    SqBackpressureRetainsOnlyDomainResponsibilityAndCanBeCancelledBeforeAcceptance) {
  socket_pair endpoints;
  io_engine engine{scripted_config()};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(endpoints.fd[0], false);
  char borrowed{};
  active_script->allow_submit = false;
  io_request request;
  request.kind = operation_kind::recv;
  request.resource = resource;
  request.buffer = &borrowed;
  request.length = 1;
  completion observed;
  int error{};
  const auto token =
      domain->prepare(std::move(request), observed.target(), error);
  ASSERT_NE(token.value, 0u);
  engine.submitter().submit(token);
  EXPECT_EQ(observed.count, 0u);
  EXPECT_TRUE(active_script->accepted.empty());
  EXPECT_EQ(active_script->counters.native_submitted, 0u);
  engine.submitter().request_cancel(token);
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 1u);
  EXPECT_EQ(observed.result, -ECANCELED);
  EXPECT_TRUE(engine.driver().quiescent());
  EXPECT_EQ(active_script->counters.cancel_ack, 0u);
}

/** @brief 取消赢得仲裁时，已经成功产生的accept/open/socket新fd必须被回收。 */
TEST(NativeCompletionContract,
     CancelledSuccessfulDescriptorCreationClosesTheUnpublishedDescriptor) {
  for (const auto kind :
       {operation_kind::accept, operation_kind::open, operation_kind::socket}) {
    socket_pair endpoints;
    io_engine engine{scripted_config()};
    auto domain = engine.context().domain();
    auto resource = domain->adopt(endpoints.fd[0], false);
    io_request request;
    request.kind = kind;
    if (kind == operation_kind::accept)
      request.resource = resource;
    if (kind == operation_kind::open) {
      request.fd = AT_FDCWD;
      request.path = "/scripted/not-executed";
      request.flags = O_RDONLY;
    }
    if (kind == operation_kind::socket) {
      request.argument = AF_INET;
      request.argument2 = SOCK_STREAM;
    }
    completion observed;
    int error{};
    const auto token =
        domain->prepare(std::move(request), observed.target(), error);
    ASSERT_NE(token.value, 0u);
    engine.submitter().submit(token);
    ASSERT_TRUE(active_script->accepted.contains(token.value));
    engine.submitter().request_cancel(token);
    (void)engine.driver().drive();
    ASSERT_EQ(observed.count, 0u);
    const int created = ::dup(endpoints.fd[1]);
    ASSERT_GE(created, 0);
    faio::io::unix::OwnedFd failed_test_cleanup{created};
    active_script->events.push_back(
        {backend_event_kind::result, token.value, created, 0});
    bool native_close_seen = false;
    // mock 不执行内核 syscall；显式模拟实际接管的 CLOSE 和对应终止 CQE，
    // 不再依赖原生路径之外的 cleanup worker 执行关闭。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while ((!observed.count || !engine.driver().quiescent()) &&
           std::chrono::steady_clock::now() < deadline) {
      (void)engine.driver().wait_and_drive(10);
      if (!native_close_seen) {
        for (const auto &[close_token, close_request] :
             active_script->accepted) {
          if (close_request->kind != operation_kind::close ||
              close_request->fd != created)
            continue;
          ASSERT_EQ(::close(created), 0);
          (void)failed_test_cleanup.release();
          active_script->events.push_back(
              {backend_event_kind::result, close_token, 0, 0});
          native_close_seen = true;
          break;
        }
      }
      if (!engine.driver().quiescent())
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(observed.count, 1u);
    EXPECT_EQ(observed.result, -ECANCELED);
    EXPECT_TRUE(native_close_seen);
    EXPECT_EQ(engine.context().cleanup().started_threads(), 0u);
    EXPECT_EQ(::fcntl(created, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_TRUE(engine.driver().quiescent());
  }
}

TEST(NativeCompletionContract,
     CancellationAfterZeroCopyResultWaitsForNotificationAndPreservesProgress) {
  socket_pair endpoints;
  io_engine engine{scripted_config()};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(endpoints.fd[0], false);
  const std::array<char, 5> borrowed{'l', 'e', 'a', 's', 'e'};
  io_request request;
  request.kind = operation_kind::send_zc;
  request.resource = resource;
  request.const_buffer = borrowed.data();
  request.length = borrowed.size();
  completion observed;
  int error{};
  const auto token =
      domain->prepare(std::move(request), observed.target(), error);
  ASSERT_NE(token.value, 0u);
  engine.submitter().submit(token);
  active_script->events.push_back(
      {backend_event_kind::result, token.value, 5, 1});
  (void)engine.driver().drive();
  ASSERT_EQ(observed.count, 0u);
  engine.submitter().request_cancel(token);
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 0u);
  EXPECT_FALSE(engine.driver().quiescent());
  active_script->events.push_back(
      {backend_event_kind::buffer_release, token.value, 0, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 1u);
  EXPECT_EQ(observed.result, -ECANCELED);
  EXPECT_EQ(observed.progress, borrowed.size());
  EXPECT_TRUE(engine.driver().quiescent());
}

/** @brief 永久flush错误先停止新提交，ACK仍不允许提前交还内核借用。 */
TEST(
    NativeCompletionContract,
    FatalFlushRetainsNativeBorrowUntilAcknowledgementAndOriginalTerminalAreDrained) {
  socket_pair endpoints;
  io_engine engine{scripted_config()};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(endpoints.fd[0], false);
  active_script->auto_terminal_on_shutdown = false;
  struct terminal_guard {
    std::shared_ptr<script> state;
    ~terminal_guard() {
      // ASSERT失败退出时也将原请求排空，否则测试会卡在故障engine的析构。
      for (const auto &[token, request] : state->accepted) {
        (void)request;
        state->events.push_back({backend_event_kind::result, token, -EIO, 0});
      }
    }
  } finally{active_script};
  char borrowed{};
  io_request request;
  request.kind = operation_kind::recv;
  request.resource = resource;
  request.buffer = &borrowed;
  request.length = 1;
  completion observed;
  int error{};
  const auto token =
      domain->prepare(std::move(request), observed.target(), error);
  ASSERT_NE(token.value, 0u);
  engine.submitter().submit(token);
  ASSERT_TRUE(active_script->accepted.contains(token.value));
  active_script->flush_error = EIO;
  const auto fatal = engine.driver().drive();
  EXPECT_EQ(fatal.fatal_error, EIO);
  EXPECT_TRUE(active_script->failure_started);
  EXPECT_TRUE(engine.context().stopped());
  EXPECT_EQ(observed.count, 0u);
  EXPECT_EQ(active_script->counters.cancel_ack, 1u);
  EXPECT_FALSE(engine.driver().quiescent());
  ASSERT_TRUE(active_script->accepted.contains(token.value));
  auto *retained = active_script->accepted.at(token.value);
  EXPECT_EQ(retained->buffer, &borrowed);
  *static_cast<char *>(retained->buffer) = 'd';
  active_script->events.push_back(
      {backend_event_kind::result, token.value, 1, 0});
  const auto drained = engine.driver().drive();
  EXPECT_EQ(drained.fatal_error, EIO);
  EXPECT_EQ(observed.count, 1u);
  EXPECT_EQ(observed.result, -EIO);
  EXPECT_EQ(observed.progress, 1u);
  EXPECT_EQ(borrowed, 'd');
  EXPECT_TRUE(engine.driver().quiescent());
}

/** @brief 合并准备/接受必须在任何原生提交以前仲裁已观察的任务停止。 */
TEST(NativeCompletionContract,
     MergedAdmissionObservesStoppedTaskBeforeNativeAcceptance) {
  socket_pair endpoints;
  io_engine engine{scripted_config()};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(endpoints.fd[0], false);
  std::stop_source stopping;
  stopping.request_stop();
  char borrowed{};
  io_request request;
  request.kind = operation_kind::recv;
  request.resource = resource;
  request.buffer = &borrowed;
  request.length = 1;
  completion observed;
  int error{};
  const auto token = domain->prepare_submit(
      std::move(request), observed.target(), error, stopping.get_token());
  ASSERT_NE(token.value, 0u);
  EXPECT_EQ(observed.count, 1u);
  EXPECT_EQ(observed.result, -ECANCELED);
  EXPECT_EQ(active_script->counters.native_submitted, 0u);
  EXPECT_TRUE(active_script->accepted.empty());
  EXPECT_TRUE(engine.driver().quiescent());
  engine.submitter().submit(token); // 即时结果已经回收，旧代际仍不能重新执行。
  EXPECT_EQ(observed.count, 1u);
}

/** @brief 已接管原生 CLOSE 不因合并入口观察到父任务停止而丢弃真实关闭责任。 */
TEST(NativeCompletionContract,
     MergedAdmissionKeepsUncancellableNativeCloseResponsibility) {
  socket_pair endpoints;
  io_engine engine{scripted_config()};
  auto domain = engine.context().domain();
  std::stop_source stopping;
  stopping.request_stop();
  io_request request;
  request.kind = operation_kind::close;
  request.fd =
      endpoints.fd[0]; // mock 保留真实 fd 生命周期；socket_pair 仍最终回收它。
  request.bypass_resource_registration = true;
  request.uncancellable = true;
  completion observed;
  int error{};
  const auto token = domain->prepare_submit(
      std::move(request), observed.target(), error, stopping.get_token());
  ASSERT_NE(token.value, 0u);
  EXPECT_EQ(observed.count, 0u);
  ASSERT_TRUE(active_script->accepted.contains(token.value));
  EXPECT_EQ(active_script->counters.native_submitted, 1u);
  engine.submitter().request_cancel(token);
  EXPECT_EQ(active_script->counters.cancel_ack, 0u);
  active_script->events.push_back(
      {backend_event_kind::result, token.value, 0, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 1u);
  EXPECT_EQ(observed.result, 0);
  EXPECT_TRUE(engine.driver().quiescent());
}

/** @brief 尚未接管责任的 typed Close 遵守任务停止，不能提前交换或关闭资源 fd。
 */
TEST(NativeCompletionContract,
     MergedCloseCancelledBeforeResponsibilityPreservesHandle) {
  socket_pair endpoints;
  io_engine engine{scripted_config()};
  auto domain = engine.context().domain();
  // 此测试不交出实际关闭责任；socket_pair 是最终唯一 fd 回收者。
  auto resource = domain->adopt(endpoints.fd[0], false);
  std::stop_source stopping;
  stopping.request_stop();
  auto request = make_request(resource, operation_kind::close);
  completion observed;
  int error{};
  const auto token = domain->prepare_submit(
      std::move(request), observed.target(), error, stopping.get_token());
  ASSERT_NE(token.value, 0u);
  EXPECT_EQ(observed.count, 1u);
  EXPECT_EQ(observed.result, -ECANCELED);
  EXPECT_EQ(active_script->counters.native_submitted, 0u);
  EXPECT_TRUE(active_script->accepted.empty());
  EXPECT_FALSE(
      resource->closing); // begin_close 尚未执行，不能留下隐藏的关闭责任。
  EXPECT_EQ(resource->fd(), endpoints.fd[0]);
  EXPECT_GE(::fcntl(endpoints.fd[0], F_GETFD),
            0); // 停止结果不能以已关闭 fd 冒充。
  EXPECT_TRUE(engine.driver().quiescent());
  engine.submitter().submit(token); // 回收后的旧代际不能稍后接管关闭责任。
  EXPECT_EQ(active_script->counters.native_submitted, 0u);
  EXPECT_EQ(observed.count, 1u);
}

/** @brief merged 接受的多个 Close waiter 在父任务停止后仍等待唯一原生 CLOSE
 * CQE。 */
TEST(NativeCompletionContract,
     MergedSharedCloseTaskStopWaitsForOriginalTerminal) {
  socket_pair endpoints;
  auto config = scripted_config();
  config.max_operations = 2;
  io_engine engine{config};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(endpoints.fd[0], true);
  std::stop_source stopping;
  completion first, second;
  int error{};
  auto first_request = make_request(resource, operation_kind::close);
  const auto first_token = domain->prepare_submit(
      std::move(first_request), first.target(), error, stopping.get_token());
  ASSERT_NE(first_token.value, 0u);
  auto second_request = make_request(resource, operation_kind::close);
  const auto second_token = domain->prepare_submit(
      std::move(second_request), second.target(), error, stopping.get_token());
  ASSERT_NE(second_token.value, 0u);
  EXPECT_EQ(first.count, 0u);
  EXPECT_EQ(second.count, 0u);
  EXPECT_EQ(active_script->counters.native_submitted, 1u);
  ASSERT_EQ(active_script->accepted.size(), 1u);
  EXPECT_TRUE(resource->closing);
  EXPECT_LT(resource->fd(),
            0); // handle 已唯一接管，尚未收到 CQE 不能交还 waiter。
  stopping.request_stop();
  // mock 不注册协程 stop_callback；这里明确投递该回调使用的同一取消入口。
  domain->request_cancel(first_token, cancel_reason::user);
  domain->request_cancel(second_token, cancel_reason::user);
  (void)engine.driver().drive();
  EXPECT_EQ(active_script->counters.cancel_ack, 0u);
  EXPECT_EQ(first.count, 0u);
  EXPECT_EQ(second.count, 0u);
  EXPECT_FALSE(engine.driver().quiescent());
  ASSERT_TRUE(active_script->accepted.contains(first_token.value));
  // 脚本先模拟内核真正完成 CLOSE，再交付原始结果，避免把 ACK 当终止或 double
  // close。
  ASSERT_EQ(::close(endpoints.fd[0]), 0);
  endpoints.fd[0] = -1;
  active_script->events.push_back(
      {backend_event_kind::result, first_token.value, 0, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(first.count, 1u);
  EXPECT_EQ(second.count, 1u);
  EXPECT_EQ(first.result, 0);
  EXPECT_EQ(second.result, 0);
  EXPECT_TRUE(engine.driver().quiescent());
  domain->request_cancel(first_token);
  engine.submitter().submit(second_token);
  EXPECT_EQ(first.count, 1u);
  EXPECT_EQ(second.count, 1u);
  EXPECT_EQ(active_script->counters.native_submitted, 1u);
}

/** @brief noncancellable 桥标志独立于
 * request.uncancellable，预停止不能跳过原生请求。 */
TEST(NativeCompletionContract,
     MergedNoncancellableBridgeIgnoresPreexistingTaskStop) {
  socket_pair endpoints;
  io_engine engine{scripted_config()};
  auto domain = engine.context().domain();
  std::stop_source stopping;
  stopping.request_stop();
  io_request request;
  request.kind = operation_kind::close;
  request.fd = endpoints.fd[0];
  request.bypass_resource_registration = true;
  // 刻意保持 uncancellable=false，单独验证桥标志不会订阅/仲裁父任务停止。
  // 真实 File Close 会额外设置 uncancellable=true；该责任分支由 J 原测试覆盖。
  completion observed;
  int error{};
  const auto token =
      domain->prepare_submit(std::move(request), observed.target(), error,
                             stopping.get_token(), false);
  ASSERT_NE(token.value, 0u);
  EXPECT_EQ(observed.count, 0u);
  EXPECT_EQ(active_script->counters.native_submitted, 1u);
  ASSERT_TRUE(active_script->accepted.contains(token.value));
  EXPECT_FALSE(engine.driver().quiescent());
  active_script->events.push_back(
      {backend_event_kind::result, token.value, 0, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 1u);
  EXPECT_EQ(observed.result, 0);
  EXPECT_TRUE(engine.driver().quiescent());
  // raw mock 不执行 syscall；socket_pair 仍持有并最终回收真实 fd。
  EXPECT_GE(::fcntl(endpoints.fd[0], F_GETFD), 0);
}

/** @brief 单槽跨种类复用必须用本代完整输入覆盖上一代控制与消息字段。 */
TEST(NativeCompletionContract,
     RecycledSlotReplacesColdAndControlFieldsAcrossKinds) {
  socket_pair endpoints;
  completion old, fresh; // 早于 engine 创建，失败排空时 consumer 仍存活。
  char byte{};           // 早于 engine 创建，失败排空时 buffer 仍可借用。
  io_engine engine{scripted_config()};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(endpoints.fd[0], false);
  int error{};
  for (const auto kind : {operation_kind::connect, operation_kind::sendto,
                          operation_kind::sendmsg}) {
    old = {};
    fresh = {};
    io_request request;
    request.kind = kind;
    request.resource = resource;
    request.const_buffer = &byte;
    request.length = 1;
    request.address.ss_family = AF_UNIX;
    request.address_length = sizeof(sockaddr_storage);
    request.vectors.push_back({&byte, 1});
    request.message.msg_iov = request.vectors.data();
    request.message.msg_iovlen = 1;
    request.scalar_vector = {&byte, 1};
    request.deadline = std::chrono::steady_clock::time_point::max();
    request.internal_control = true;
    request.uncancellable = true;
    request.bypass_resource_registration = true;
    request.observed_would_block = true;
    request.observed_readiness_generation = 71;
    request.empty_success = true;
    request.reservation =
        &byte; // 只留下请求身份值，没有设置资源上的 reservation。
    request.offset = 29;
    request.flags = MSG_PEEK;
    request.argument = 31;
    request.argument2 = 37;
    request.argument3 = 41;
    request.native_completion_key =
        91; // mock 不设置真实后端键，故意留下上一代非零值。
    request.extension_flags = 17;
    request.extension_mode = 19;
    request.extension_resolve = 23;
    const auto retired =
        domain->prepare_submit(std::move(request), old.target(), error);
    ASSERT_NE(retired.value, 0u);
    ASSERT_TRUE(active_script->accepted.contains(retired.value));
    const auto slot_address = active_script->accepted.at(retired.value);
    active_script->events.push_back({backend_event_kind::result, retired.value,
                                     kind == operation_kind::connect ? 0 : 1,
                                     0});
    (void)engine.driver().drive();
    ASSERT_EQ(old.count, 1u);
    ASSERT_TRUE(engine.driver().quiescent());

    io_request scalar;
    scalar.kind = operation_kind::recv;
    scalar.resource = resource;
    scalar.buffer = &byte;
    scalar.length = 1;
    const auto current =
        domain->prepare_submit(std::move(scalar), fresh.target(), error);
    ASSERT_NE(current.value, 0u);
    ASSERT_NE(current.value, retired.value);
    ASSERT_TRUE(active_script->accepted.contains(current.value));
    const auto *owned = active_script->accepted.at(current.value);
    EXPECT_EQ(owned, slot_address); // capacity=1，实际复用了上一代稳定对象。
    EXPECT_FALSE(owned->internal_control);
    EXPECT_FALSE(owned->uncancellable);
    EXPECT_FALSE(owned->bypass_resource_registration);
    EXPECT_FALSE(owned->observed_would_block);
    EXPECT_FALSE(owned->empty_success);
    EXPECT_FALSE(owned->establish_reservation);
    EXPECT_EQ(owned->reservation, nullptr);
    EXPECT_EQ(owned->offset, UINT64_MAX);
    EXPECT_EQ(owned->flags | owned->argument | owned->argument2 |
                  owned->argument3,
              0);
    EXPECT_EQ(owned->observed_readiness_generation, 0u);
    EXPECT_EQ(owned->native_completion_key, 0u);
    EXPECT_FALSE(owned->deadline);
    EXPECT_EQ(owned->address.ss_family, 0);
    EXPECT_EQ(owned->address_length, 0u);
    EXPECT_EQ(owned->message.msg_iov, nullptr);
    EXPECT_EQ(owned->message.msg_iovlen, 0u);
    EXPECT_EQ(owned->scalar_vector.iov_base, nullptr);
    EXPECT_EQ(owned->scalar_vector.iov_len, 0u);
    EXPECT_TRUE(owned->vectors.empty());
    EXPECT_EQ(owned->extension_flags | owned->extension_mode |
                  owned->extension_resolve,
              0u);
    engine.submitter().request_cancel(
        current); // 上一代 uncancellable 不能阻止本代取消。
    (void)engine.driver().drive();
    EXPECT_EQ(fresh.count, 0u); // ACK 仍不能当作原请求终止。
    active_script->events.push_back(
        {backend_event_kind::result, current.value, -ECANCELED, 0});
    (void)engine.driver().drive();
    EXPECT_EQ(fresh.count, 1u);
    EXPECT_EQ(fresh.result, -ECANCELED);
  }
  EXPECT_TRUE(engine.driver().quiescent());
}

/** @brief MORE/ACK/NOTIF、回调重入与大拥有参数必须共同遵守最终回收边界。 */
TEST(NativeCompletionContract,
     RecycledOwnedParametersReleaseOnlyAfterFinalNotificationAndCallback) {
  socket_pair endpoints;
  struct callback_state {
    std::shared_ptr<io_domain> domain;
    std::weak_ptr<resource_state> witness;
    io_request *stable{};
    completion observed;
    int descriptor{-1};
    char byte{};
    completion_target target() noexcept {
      return {this, +[](void *pointer, std::int64_t result,
                        std::uint64_t progress) noexcept {
                auto &self = *static_cast<callback_state *>(pointer);
                ++self.observed.count;
                self.observed.result = result;
                self.observed.progress = progress;
                EXPECT_FALSE(
                    self.witness
                        .expired()); // callback 返回前 owning 租约尚未释放。
                EXPECT_EQ(self.stable->vectors.size(), 1024u);
                EXPECT_EQ(self.stable->path.size(), 4096u);
                EXPECT_EQ(self.stable->path2.size(), 8192u);
                io_request reentrant;
                reentrant.kind = operation_kind::recv;
                reentrant.fd = self.descriptor;
                reentrant.bypass_resource_registration = true;
                reentrant.buffer = &self.byte;
                reentrant.length = 1;
                int error{};
                EXPECT_EQ(
                    self.domain->prepare(std::move(reentrant), {}, error).value,
                    0u);
                EXPECT_EQ(
                    error,
                    EAGAIN); // 当前 publisher 仍拥有唯一槽，不能在回调里复用。
              }};
    }
  } callback; // 所有 consumer 早于 engine 创建，ASSERT 失败排空也不会悬空。
  completion fresh;
  const std::array<char, 5> payload{'o', 'w', 'n', 'e', 'd'};
  io_engine engine{scripted_config()};
  callback.domain = engine.context().domain();
  callback.descriptor = endpoints.fd[0];
  auto resource = callback.domain->adopt(endpoints.fd[0], false);
  callback.witness = resource;
  io_request request;
  request.kind = operation_kind::sendmsg_zc;
  request.resource = resource;
  request.vectors.assign(
      1024, iovec{const_cast<char *>(payload.data()), payload.size()});
  request.message.msg_iov = request.vectors.data();
  request.message.msg_iovlen = request.vectors.size();
  request.path.assign(4096, 'p');
  request.path2.assign(8192, 'q');
  int error{};
  const auto retired = callback.domain->prepare_submit(
      std::move(request), callback.target(), error);
  ASSERT_NE(retired.value, 0u);
  ASSERT_TRUE(active_script->accepted.contains(retired.value));
  callback.stable = active_script->accepted.at(retired.value);
  resource.reset(); // 此后只由稳定请求拥有资源，weak 真实观察回收边界。
  active_script->events.push_back(
      {backend_event_kind::result, retired.value, 5, 1});
  (void)engine.driver().drive();
  EXPECT_EQ(callback.observed.count, 0u);
  EXPECT_FALSE(callback.witness.expired());
  EXPECT_EQ(callback.stable->vectors.size(), 1024u);
  engine.submitter().request_cancel(retired);
  (void)engine.driver().drive();
  EXPECT_EQ(callback.observed.count, 0u);
  EXPECT_FALSE(callback.witness.expired());
  EXPECT_EQ(callback.stable->path2.size(), 8192u);
  active_script->events.push_back(
      {backend_event_kind::buffer_release, retired.value, 0, 0});
  (void)engine.driver().drive();
  ASSERT_EQ(callback.observed.count, 1u);
  EXPECT_EQ(callback.observed.result, -ECANCELED);
  EXPECT_EQ(callback.observed.progress, payload.size());
  ASSERT_TRUE(
      engine.driver()
          .quiescent()); // callback/publisher 全部返回以后再判断 owning 释放。
  EXPECT_TRUE(callback.witness.expired());
  // 域仍拥有固定槽对象；只读还活着的 owning 容器，不能把 free 残值当作有效 IO
  // 参数。
  EXPECT_EQ(callback.stable->vectors.capacity(),
            std::vector<iovec>{}.capacity());
  EXPECT_EQ(callback.stable->path.capacity(), std::string{}.capacity());
  EXPECT_EQ(callback.stable->path2.capacity(), std::string{}.capacity());

  io_request scalar;
  scalar.kind = operation_kind::recv;
  scalar.fd = endpoints.fd[0];
  scalar.bypass_resource_registration = true;
  scalar.buffer = &callback.byte;
  scalar.length = 1;
  const auto current =
      callback.domain->prepare_submit(std::move(scalar), fresh.target(), error);
  ASSERT_NE(current.value, 0u);
  ASSERT_TRUE(active_script->accepted.contains(current.value));
  EXPECT_EQ(active_script->accepted.at(current.value), callback.stable);
  active_script->events.push_back(
      {backend_event_kind::cancel_ack, retired.value, 0, 0});
  active_script->events.push_back(
      {backend_event_kind::buffer_release, retired.value, 0, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(fresh.count, 0u); // 旧代际 late ACK/NOTIF 不能清理或恢复本代。
  ASSERT_TRUE(active_script->accepted.contains(current.value));
  *static_cast<char *>(active_script->accepted.at(current.value)->buffer) = 'n';
  active_script->events.push_back(
      {backend_event_kind::result, current.value, 1, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(fresh.count, 1u);
  EXPECT_EQ(callback.byte, 'n');
  EXPECT_TRUE(engine.driver().quiescent());
}

/** @brief 紧凑请求物化后仍由稳定槽持有全部原生输入，复用槽不遗留上一代冷字段。
 */
TEST(NativeCompletionContract,
     CompactScalarMaterializesOwnedStableInputAndReusesOneSlot) {
  socket_pair endpoints;
  std::array<char, 4> bytes{'n', 'a', 't', 'v'};
  completion received, sent;
  io_engine engine{scripted_config()};
  auto domain = engine.context().domain();
  auto resource = domain->adopt(endpoints.fd[0], false);
  char owner{};
  scalar_io_request compact;
  compact.kind = operation_kind::recv;
  compact.resource = resource;
  compact.buffer = bytes.data();
  compact.length = bytes.size();
  compact.flags = MSG_PEEK;
  compact.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
  compact.bypass_resource_registration = true;
  compact.establish_reservation = true;
  compact.reservation = &owner;
  ASSERT_EQ(::send(endpoints.fd[1], "r", 1, 0), 1);
  EXPECT_FALSE(
      domain->try_immediate(compact, false)); // native 绝不执行前置 recv。
  char unchanged{};
  ASSERT_EQ(::recv(endpoints.fd[0], &unchanged, 1, MSG_PEEK), 1);
  EXPECT_EQ(unchanged, 'r');
  auto full = std::move(compact).into_request();
  int error{};
  const auto first =
      domain->prepare_submit(std::move(full), received.target(), error);
  ASSERT_NE(first.value, 0u);
  ASSERT_TRUE(active_script->accepted.contains(first.value));
  const auto *stable = active_script->accepted.at(first.value);
  EXPECT_EQ(stable->resource, resource);
  EXPECT_EQ(stable->kind, operation_kind::recv);
  EXPECT_EQ(stable->buffer, bytes.data());
  EXPECT_EQ(stable->flags, MSG_PEEK);
  EXPECT_EQ(stable->length, bytes.size());
  EXPECT_EQ(stable->deadline, compact.deadline);
  EXPECT_TRUE(stable->bypass_resource_registration);
  EXPECT_EQ(resource->read_reserved, &owner);
  EXPECT_EQ(stable->output_message, nullptr);
  EXPECT_TRUE(stable->vectors.empty());
  EXPECT_TRUE(stable->path.empty());
  EXPECT_TRUE(stable->path2.empty());
  EXPECT_EQ(stable->offset, UINT64_MAX);
  active_script->events.push_back(
      {backend_event_kind::result, first.value, 4, 0});
  (void)engine.driver().drive();
  ASSERT_EQ(received.count, 1u);
  EXPECT_EQ(received.result, 4);
  domain->unreserve(*resource, Interest::readable, &owner);
  scalar_io_request next;
  next.kind = operation_kind::send;
  next.resource = resource;
  next.const_buffer = bytes.data();
  next.length = bytes.size();
  next.flags = MSG_DONTWAIT;
  auto next_full = std::move(next).into_request();
  const auto second =
      domain->prepare_submit(std::move(next_full), sent.target(), error);
  ASSERT_NE(second.value, 0u);
  EXPECT_EQ(static_cast<std::uint32_t>(second.value),
            static_cast<std::uint32_t>(first.value));
  EXPECT_GT(second.value, first.value);
  ASSERT_TRUE(active_script->accepted.contains(second.value));
  const auto *new_stable = active_script->accepted.at(second.value);
  EXPECT_EQ(new_stable->const_buffer, bytes.data());
  EXPECT_EQ(new_stable->buffer, nullptr);
  EXPECT_FALSE(new_stable->deadline);
  EXPECT_EQ(new_stable->reservation, nullptr);
  EXPECT_FALSE(new_stable->bypass_resource_registration);
  EXPECT_EQ(new_stable->flags, MSG_DONTWAIT);
  domain->request_cancel(first); // 前代际不能更改新发送的 sticky cancel。
  active_script->events.push_back(
      {backend_event_kind::result, second.value, 4, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(sent.count, 1u);
  EXPECT_EQ(sent.result, 4);
}

/** @brief 本地一次 CQ 机会领取终态后，callback 必须已退出自己的 driver
 * session。 */
TEST(NativeCompletionContract,
     LocalCQClaimPublishesAfterDriverSessionAndKeepsSlotThroughCallback) {
  socket_pair endpoints;
  struct callback_state {
    std::shared_ptr<io_domain> domain;
    io_driver_ref driver;
    std::weak_ptr<resource_state> witness;
    int descriptor{-1};
    char byte{};
    unsigned count{};
    completion_target target() noexcept {
      return {
          this,
          +[](void *pointer, std::int64_t result, std::uint64_t) noexcept {
            auto &self = *static_cast<callback_state *>(pointer);
            ++self.count;
            EXPECT_EQ(result, 1);
            EXPECT_EQ(current_driver_domain,
                      nullptr); // callback 不能借用已经结束的低 load session。
            EXPECT_EQ(self.driver.drive().fatal_error,
                      0); // 能立即取得原唯一 CQ driver，不得返回 EBUSY。
            EXPECT_FALSE(
                self.witness.expired()); // 原稳定请求仍拥有 resource，直到本
                                         // callback 返回。
            io_request next;
            next.kind = operation_kind::recv;
            next.fd = self.descriptor;
            next.bypass_resource_registration = true;
            next.buffer = &self.byte;
            next.length = 1;
            int error{};
            EXPECT_EQ(self.domain->prepare(std::move(next), {}, error).value,
                      0u);
            EXPECT_EQ(
                error,
                EAGAIN); // 唯一槽仍被 publisher 领取，不能在 callback 中复用。
          }};
    }
  } callback; // consumer 早于 engine，ASSERT
              // 失败时原排空路径也不会借用已销毁地址。
  io_engine engine{scripted_config()};
  io_engine::binding binding{
      engine}; // 真进入生产的本地 native CQ 机会，而不是只测 normal drive。
  callback.domain = engine.context().domain();
  callback.driver = engine.driver();
  callback.descriptor = endpoints.fd[0];
  auto resource = callback.domain->adopt(endpoints.fd[0], false);
  callback.witness = resource;
  io_request request;
  request.kind = operation_kind::recv;
  request.resource = std::move(resource);
  request.buffer = &callback.byte;
  request.length = 1;
  int error{};
  const auto token =
      callback.domain->prepare(std::move(request), callback.target(), error);
  ASSERT_NE(token.value, 0u);
  active_script->events.push_back(
      {backend_event_kind::result, token.value, 1, 0});
  engine.submitter().submit(
      token); // 原后端 poll 输出后由 complete_native/claim 处理。
  EXPECT_EQ(callback.count, 1u);
  EXPECT_TRUE(
      callback.witness
          .expired()); // callback 返回后原 recycle 才解除 owning lease。
  EXPECT_TRUE(engine.driver().quiescent());
  (void)engine.driver().drive();
  EXPECT_EQ(callback.count, 1u); // 摘链与正常 driver 不能再次交付同 token。
}

/** @brief 本地一次 CQ 机会中的 MORE 仍不可领取，ACK 也不能结束真正的 buffer
 * lease。 */
TEST(NativeCompletionContract,
     LocalCQClaimWaitsThroughMoreAndAckUntilFinalBufferRelease) {
  socket_pair endpoints;
  completion observed; // consumer 与 payload 均早于 engine，覆盖失败排空路径。
  const char payload = 'x';
  io_engine engine{scripted_config()};
  io_engine::binding binding{engine};
  auto resource = engine.context().domain()->adopt(endpoints.fd[0], false);
  std::weak_ptr<resource_state> witness = resource;
  io_request request;
  request.kind = operation_kind::send_zc;
  request.resource = std::move(resource);
  request.const_buffer = &payload;
  request.length = 1;
  int error{};
  const auto token = engine.context().domain()->prepare(
      std::move(request), observed.target(), error);
  ASSERT_NE(token.value, 0u);
  active_script->events.push_back(
      {backend_event_kind::result, token.value, 1, 1});
  engine.submitter().submit(
      token); // 本地 poll 消费 MORE，queued_completion 必须仍为 false。
  EXPECT_EQ(observed.count, 0u);
  EXPECT_FALSE(witness.expired());
  active_script->events.push_back(
      {backend_event_kind::cancel_ack, token.value, 0, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 0u);
  EXPECT_FALSE(witness.expired());
  active_script->events.push_back(
      {backend_event_kind::buffer_release, token.value, 0, 0});
  (void)engine.driver().drive();
  EXPECT_EQ(observed.count, 1u);
  EXPECT_EQ(observed.result,
            1); // 最终 NOTIF 使用原发送结果，不能以零通知结果改写它。
  EXPECT_TRUE(witness.expired());
  EXPECT_TRUE(engine.driver().quiescent());
}
