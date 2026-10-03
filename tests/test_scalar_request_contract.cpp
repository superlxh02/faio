/**
 * @file test_scalar_request_contract.cpp
 * @brief 紧凑 RECV/SEND 的真实 socket、稳定槽、取消和拥有语义回归。
 * @details runtime 测试按原 Linux 双后端/current_thread/multi_thread 矩阵运行；
 *          瞬时路径专用 engine 明确选择 readiness，原生提交仍由真实矩阵验证。
 */
#include "backend_test_support.hpp"
#include "test_support.hpp"
#include <array>
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <sys/socket.h>
#include <type_traits>
#include <vector>

namespace compact_scalar_test {
using namespace std::chrono_literals;
using namespace faio::io::detail;
using faio_test::take;

struct default_bridge : IORegistrantAwaiter<default_bridge> {
  using IORegistrantAwaiter<default_bridge>::IORegistrantAwaiter;
};
static_assert(sizeof(faio::io::detail::Recv) < sizeof(default_bridge));
static_assert(sizeof(faio::io::detail::Send) < sizeof(default_bridge));
static_assert(std::is_base_of_v<IORegistrantAwaiter<faio::io::detail::SendZC>,
                                faio::io::detail::SendZC>);
static_assert(std::is_same_v<
              decltype(std::declval<faio::io::detail::Recv &&>().set_timeout_at(
                  std::chrono::steady_clock::now())),
              faio::io::detail::Recv>);
static_assert(std::is_same_v<decltype(std::declval<faio::io::detail::Send &>()
                                          .establish_reservation(nullptr)),
                             faio::io::detail::Send &>);

template <class T>
concept timeout_mutable = requires(T &&operation) {
  faio::time::timeout(std::forward<T>(operation), 1ms);
  faio::time::timeout_at(std::forward<T>(operation),
                         std::chrono::steady_clock::now());
};
static_assert(timeout_mutable<Recv> && timeout_mutable<Recv &>);
static_assert(timeout_mutable<Send> && timeout_mutable<Send &>);
static_assert(timeout_mutable<Read> && timeout_mutable<Read &>);
static_assert(!timeout_mutable<const Recv &>);
static_assert(
    std::same_as<decltype(faio::time::timeout(std::declval<Recv &&>(), 1ms)),
                 Recv>);
static_assert(
    std::same_as<decltype(faio::time::timeout(std::declval<Send &>(), 1ms)),
                 Send &>);
static_assert(
    std::same_as<decltype(faio::time::timeout_at(
                     std::declval<Recv &>(), std::chrono::steady_clock::now())),
                 Recv &>);
static_assert(std::same_as<decltype(faio::time::timeout_at(
                               std::declval<Send &&>(),
                               std::chrono::steady_clock::now())),
                           Send>);
using timed_receive = faio::time::detail::Timeout<Recv>;
using timed_send = faio::time::detail::Timeout<Send>;
static_assert(io_registrant_operation<timed_receive> &&
              io_registrant_operation<timed_send>);
static_assert(std::is_move_constructible_v<timed_receive> &&
              !std::is_copy_constructible_v<timed_receive>);

/** @brief 原生描述符在 engine 及其全部 stable buffer 引用排空后才关闭。 */
struct descriptor_pair {
  std::array<faio::net::detail::owned_native_socket, 2> owned;
  descriptor_pair() {
    std::array<int, 2> fd{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fd.data()))
      throw std::system_error(errno, std::generic_category());
    owned[0] = faio::net::detail::owned_native_socket{fd[0]};
    owned[1] = faio::net::detail::owned_native_socket{fd[1]};
    take(faio::net::detail::Socket::prepare(fd[0]));
    take(faio::net::detail::Socket::prepare(fd[1]));
  }
  int fd(std::size_t index) const noexcept { return owned[index].get(); }
};
struct observed {
  unsigned count{};
  std::int64_t result{};
  faio::io::detail::completion_target target() noexcept {
    return {this,
            +[](void *pointer, std::int64_t result, std::uint64_t) noexcept {
              auto &self = *static_cast<observed *>(pointer);
              ++self.count;
              self.result = result;
            }};
  }
};
scalar_io_request read_request(resource_ptr resource, char &byte) {
  scalar_io_request request;
  request.kind = operation_kind::recv;
  request.resource = std::move(resource);
  request.buffer = &byte;
  request.length = 1;
  return request;
}

/** @brief 只执行显式收到的恢复，不直接恢复已挂起帧；支持桥的原 schedule_io
 * 回退。 */
struct manual_scheduler {
  std::deque<std::coroutine_handle<>> ready;
  void enqueue(std::coroutine_handle<> handle) { ready.push_back(handle); }
  void pump() {
    while (!ready.empty()) {
      const auto handle = ready.front();
      ready.pop_front();
      faio::detail::cooperative_poll_scope poll;
      handle.resume();
    }
  }
};
using receive_result = faio::expected<std::size_t>;
/** @brief 外部右值必须被 scoped_awaiter 移入帧，外部对象可在 IO pending
 * 后销毁。 */
faio::task<receive_result> consume_external(faio::io::detail::Recv &external) {
  co_return co_await std::move(external);
}
faio::task<receive_result> consume_owned(faio::io::detail::Recv operation) {
  co_return co_await operation; // 参数按值拥有，命名 awaiter 借用覆盖整个等待。
}
faio::task<receive_result> consume_borrowed(faio::io::detail::Recv &operation) {
  co_return co_await operation; // 未被接受时，调用方的命名 awaiter
                                // 必须仍保有资源租约。
}
struct frame_owner {
  faio::task<receive_result>::handle_type handle;
  faio::io::io_engine &engine;
  manual_scheduler &scheduler;
  ~frame_owner() {
    if (!handle)
      return;
    if (!handle.done()) {
      engine.begin_shutdown(faio::io::shutdown_policy::cancel_all);
      for (unsigned attempt = 0; attempt < 100 && !handle.done(); ++attempt) {
        (void)engine.driver().wait_and_drive(10);
        scheduler.pump();
      }
      if (!handle.done())
        std::terminate(); // 不能销毁仍被真实内核借用的 awaiter。
    }
    handle.destroy();
  }
};

/** @brief 真实 UDP 零长报文必须消费一次；下一包带字节，不能被前一空读隐藏。 */
faio::task<bool> udp_zero_and_next_payload() {
  auto left = take(faio::net::UdpSocket::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  auto right = take(faio::net::UdpSocket::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  take(co_await left.connect(take(right.local_addr())));
  take(co_await right.connect(take(left.local_addr())));
  if (take(co_await faio::io::send(left.resource(), nullptr, 0, 0)) != 0)
    co_return false;
  constexpr char expected = 'u';
  if (take(co_await faio::io::send(left.resource(), &expected, 1,
                                   MSG_DONTWAIT)) != 1)
    co_return false;
  char byte{};
  // 空数据报与空接收缓冲不是同一语义：Darwin recv(..., 0) 不消费队首报文。
  // 给出真实接收容量，仍要求返回 0，再用下一包验证空数据报确实被消费。
  auto zero =
      co_await faio::io::recv(right.resource(), &byte, 1, 0).set_timeout(1s);
  if (!zero || *zero != 0)
    co_return false;
  auto next =
      co_await faio::io::recv(right.resource(), &byte, 1, 0).set_timeout(1s);
  co_return next && *next == 1 && byte == expected;
}

faio::task<bool> tcp_read_large_with_peek(faio::net::TcpListener listener,
                                          std::vector<char> expected) {
  auto accepted = take(co_await listener.accept().set_timeout(2s));
  auto stream = std::move(accepted.first);
  std::array<char, 16> peeked{};
  faio::expected<std::size_t> peek = std::unexpected{faio::make_error(EAGAIN)};
  const auto until = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < until) {
    peek = co_await faio::io::recv(stream.resource(), peeked.data(),
                                   peeked.size(), MSG_PEEK | MSG_DONTWAIT)
               .set_timeout(2s);
    if (peek || peek.error().value() != EAGAIN)
      break;
    co_await faio::time::sleep(
        1ms); // 原生 MSG_DONTWAIT 可合法交付 EAGAIN，不修改生产语义。
  }
  if (!peek || *peek == 0 || *peek > peeked.size())
    co_return false;
  for (std::size_t i = 0; i < *peek; ++i)
    if (peeked[i] != expected[i])
      co_return false;
  std::vector<char> actual(expected.size());
  std::size_t offset{};
  while (offset < actual.size()) {
    auto result =
        co_await faio::io::recv(stream.resource(), actual.data() + offset,
                                actual.size() - offset, MSG_DONTWAIT)
            .set_timeout(2s);
    if (!result && result.error().value() == EAGAIN &&
        std::chrono::steady_clock::now() < until) {
      co_await faio::time::sleep(1ms);
      continue;
    }
    if (!result || *result == 0)
      co_return false;
    offset += *result; // peek 不消费，必须从第一个字节开始完整读取。
  }
  co_return actual == expected;
}
/** @brief 真实 TCP 多次短写、MSG_PEEK、MSG_DONTWAIT 和真实原生计数。
 * @details 不要求内核每次短写；缩小发送缓冲后使用 2MiB
 * payload，所有结果逐字节验证。
 */
faio::task<bool> tcp_large_scalar_flags() {
  auto listener = take(faio::net::TcpListener::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  std::vector<char> payload(2 * 1024 * 1024);
  for (std::size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<char>('a' + (i % 23));
  const auto address = take(listener.local_addr());
  // peer 参数在调用时按值保存，不因客户端异常或取消而借用已销毁的父局部对象。
  auto peer =
      faio::spawn(tcp_read_large_with_peek(std::move(listener), payload));
  auto stream = take(co_await faio::net::TcpStream::connect(address));
  take(stream.set_nodelay(
      true)); // 避免测试将正常 Nagle/ACK 定时器误认作 scalar 协议失败。
  take(stream.set_send_buffer_size(4096));
  const auto domain = stream.resource()->owner.get();
  const bool native = domain->supports_native(operation_kind::send);
  const auto before = domain->statistics().native_submitted;
  std::size_t offset{};
  const auto until = std::chrono::steady_clock::now() + 10s;
  while (offset < payload.size()) {
    auto result =
        co_await faio::io::send(stream.resource(), payload.data() + offset,
                                payload.size() - offset, MSG_DONTWAIT)
            .set_timeout(2s);
    if (!result && result.error().value() == EAGAIN &&
        std::chrono::steady_clock::now() < until) {
      co_await faio::time::sleep(1ms);
      continue;
    }
    if (!result || *result == 0) {
      peer.request_stop();
      (void)co_await peer;
      co_return false;
    }
    offset += *result;
    co_await faio::this_coro::yield_if_needed();
  }
  const bool received =
      co_await peer; // 检查任何统计失败以前也先排空实际 peer。
  co_return received &&
      (!native || domain->statistics().native_submitted > before);
}

/** @brief 尚未绑定的包装销毁后，按值 Send 参数仍保有调用时控制块。 */
faio::task<bool> complete_deferred_send(faio::io::detail::Send operation,
                                        faio::net::unix::UnixStream &peer) {
  auto result = co_await std::move(operation);
  if (!result || *result != 4)
    co_return false;
  std::array<char, 4> actual{};
  take(co_await peer.read_exact(actual));
  co_return actual == std::array<char, 4>{'o', 'w', 'n', '!'};
}
/** @brief timeout 的右值仍移动拥有，外部 Timeout 源在真实挂起后可以销毁。 */
faio::task<receive_result> consume_external_timeout(timed_receive &external) {
  co_return co_await faio::time::timeout(std::move(external), 1s);
}

/** @brief compact/full 两策略均经真实 socket 验证 helper 值类别及兼容包装。 */
faio::task<bool> timeout_helpers_transfer_compact_and_full() {
  auto endpoints = take(faio::net::unix::UnixStream::pair());
  char payload = 'r', byte{};
  // 未命名右值经 helper 返回拥有型值；实际发送与实际接收均检查字节。
  if (take(co_await faio::time::timeout(
          faio::io::send(endpoints.first.resource(), &payload, 1, 0), 1s)) != 1)
    co_return false;
  if (take(co_await faio::time::timeout_at(
          faio::io::recv(endpoints.second.resource(), &byte, 1, 0),
          std::chrono::steady_clock::now() + 1s)) != 1 ||
      byte != payload)
    co_return false;
  payload = 'l';
  auto sending = faio::io::send(endpoints.first.resource(), &payload, 1, 0);
  auto receiving = faio::io::recv(endpoints.second.resource(), &byte, 1, 0);
  auto &configured_send =
      faio::time::timeout_at(sending, std::chrono::steady_clock::now() + 1s);
  auto &configured_recv = faio::time::timeout(receiving, 1s);
  if (&configured_send != &sending || &configured_recv != &receiving)
    co_return false; // 左值的返回必须是原对象，不能复制或隐式消费它。
  if (take(co_await configured_send) != 1 ||
      take(co_await configured_recv) != 1 || byte != payload)
    co_return false;
  payload = 'w';
  timed_send wrapped_send{faio::time::timeout(
      faio::io::send(endpoints.first.resource(), &payload, 1, 0), 1s)};
  timed_receive wrapped_recv{faio::time::timeout_at(
      faio::io::recv(endpoints.second.resource(), &byte, 1, 0),
      std::chrono::steady_clock::now() + 1s)};
  if (take(co_await std::move(wrapped_send)) != 1 ||
      take(co_await std::move(wrapped_recv)) != 1 || byte != payload)
    co_return false; // 实际 await Timeout<T>，不只形成未使用的类型别名。
  payload = 'f';
  auto generic_send = faio::io::write(endpoints.first.resource(), &payload, 1);
  auto &full_reference = faio::time::timeout(generic_send, 1s);
  if (&full_reference != &generic_send || take(co_await full_reference) != 1)
    co_return false;
  faio::time::detail::Timeout<Read> full_recv{faio::time::timeout_at(
      faio::io::read(endpoints.second.resource(), &byte, 1),
      std::chrono::steady_clock::now() + 1s)};
  co_return take(co_await std::move(full_recv)) == 1 && byte == payload;
}

/** @brief helper 保留真实 pending 超时、调用时 deadline 与失败无 payload
 * 副作用。 */
faio::task<bool> timeout_helpers_deadline_and_reuse() {
  auto endpoints = take(faio::net::unix::UnixStream::pair());
  char byte{}, rejected = 'x', accepted = 'g';
  auto pending = co_await faio::time::timeout(
      faio::io::recv(endpoints.second.resource(), &byte, 1, 0), 20ms);
  if (pending || pending.error().value() != ETIMEDOUT)
    co_return false;
  timed_receive absolute{faio::time::timeout_at(
      faio::io::recv(endpoints.second.resource(), &byte, 1, 0),
      std::chrono::steady_clock::now() + 20ms)};
  auto wrapped = co_await std::move(absolute);
  if (wrapped || wrapped.error().value() != ETIMEDOUT)
    co_return false;
  auto expired_send = co_await faio::time::timeout_at(
      faio::io::send(endpoints.first.resource(), &rejected, 1, 0),
      std::chrono::steady_clock::now() - 1ms);
  if (expired_send || expired_send.error().value() != ETIMEDOUT)
    co_return false;
  timed_send zero{faio::time::timeout(
      faio::io::send(endpoints.first.resource(), &rejected, 1, 0), 0ms)};
  auto zero_result = co_await std::move(zero);
  if (zero_result || zero_result.error().value() != ETIMEDOUT)
    co_return false;
  auto full_expired = co_await faio::time::timeout(
      faio::io::write(endpoints.first.resource(), &rejected, 1), 0ms);
  if (full_expired || full_expired.error().value() != ETIMEDOUT)
    co_return false;
  if (take(co_await faio::time::timeout(
          faio::io::send(endpoints.first.resource(), &accepted, 1, 0), 1s)) !=
      1)
    co_return false;
  if (take(co_await faio::time::timeout(
          faio::io::read(endpoints.second.resource(), &byte, 1), 1s)) != 1 ||
      byte != accepted)
    co_return false; // 已拒绝 SEND/WRITE 必须没有留下前置 'x'，读方向已释放。
  auto created_earlier = faio::time::timeout(
      faio::io::recv(endpoints.second.resource(), &byte, 1, 0), 10ms);
  co_await faio::time::sleep(20ms);
  accepted = 'd';
  take(co_await endpoints.first.write_all(std::span<const char>{&accepted, 1}));
  auto delayed = co_await created_earlier;
  if (delayed || delayed.error().value() != ETIMEDOUT)
    co_return false;
  // await 时不能重新起算相对超时；过期拒绝不得消耗已经到达的 payload。
  co_return take(co_await faio::time::timeout_at(
      faio::io::recv(endpoints.second.resource(), &byte, 1, 0),
      std::chrono::steady_clock::now() + 1s)) == 1 &&
      byte == accepted;
}

faio::task<bool>
complete_deferred_timed_send(timed_send operation,
                             faio::net::unix::UnixStream &peer) {
  if (take(co_await std::move(operation)) != 1)
    co_return false;
  char actual{};
  take(co_await peer.read_exact(std::span<char>{&actual, 1}));
  co_return actual == 's';
}
faio::task<bool>
complete_deferred_timed_recv(timed_receive operation,
                             faio::net::unix::UnixStream &peer) {
  const char payload = 'r';
  take(co_await peer.write_all(std::span<const char>{&payload, 1}));
  co_return take(co_await std::move(operation)) == 1;
}
} // namespace compact_scalar_test

TEST(CompactScalarContract,
     ImmediateValidationDeadlineStopAndReservationDoNotConsumePayload) {
  using namespace compact_scalar_test;
  descriptor_pair pair;
  char byte{};
  auto config = faio_test::provider_engine_config();
  faio::io::io_engine engine{config};
  const auto domain = engine.context().domain();
  const auto resource = domain->adopt(pair.fd(0), false);
  ASSERT_EQ(::send(pair.fd(1), "r", 1, 0), 1);
  auto request = read_request(resource, byte);
  auto stopped = domain->try_immediate(request, true);
  ASSERT_TRUE(stopped);
  EXPECT_EQ(stopped->result, -ECANCELED);
  request.validation_error = EINVAL;
  auto invalid = domain->try_immediate(request, false);
  ASSERT_TRUE(invalid);
  EXPECT_EQ(invalid->result, -EINVAL);
  request.validation_error = 0;
  request.deadline = std::chrono::steady_clock::now() - 1ms;
  auto expired = domain->try_immediate(request, false);
  ASSERT_TRUE(expired);
  EXPECT_EQ(expired->result, -ETIMEDOUT);
  request.deadline.reset();
  request.establish_reservation = true;
  auto identity_error = domain->try_immediate(request, false);
  ASSERT_TRUE(identity_error);
  EXPECT_EQ(identity_error->result, -EINVAL);
  char owner{}, other{};
  ASSERT_TRUE(domain->reserve(*resource, faio::io::Interest::readable, &owner));
  request.reservation = &other;
  auto busy = domain->try_immediate(request, false);
  ASSERT_TRUE(busy);
  EXPECT_EQ(busy->result, -EBUSY);
  domain->unreserve(*resource, faio::io::Interest::readable, &owner);
  request.establish_reservation = false;
  request.reservation = nullptr;
  auto received = domain->try_immediate(request, false);
  ASSERT_TRUE(received);
  EXPECT_EQ(received->result, 1);
  EXPECT_EQ(byte, 'r'); // 前面所有拒绝必须没有消费同一个真实 payload。
}

TEST(CompactScalarContract,
     WouldBlockMaterializationHonorsPoolFullAndNextGeneration) {
  using namespace compact_scalar_test;
  descriptor_pair first, second;
  char byte{}, blocked{};
  observed received, rejected, next;
  auto config = faio_test::provider_engine_config();
  config.max_operations = 1;
  faio::io::io_engine engine{config};
  const auto domain = engine.context().domain();
  auto input = domain->adopt(first.fd(0), false);
  auto other = domain->adopt(second.fd(0), false);
  auto request = read_request(input, byte);
  EXPECT_FALSE(domain->try_immediate(request, false));
  EXPECT_TRUE(request.observed_would_block);
  int error{};
  auto full = std::move(request).into_request();
  const auto old =
      domain->prepare_submit(std::move(full), received.target(), error);
  ASSERT_NE(old.value, 0u);
  auto waiting = read_request(other, blocked);
  EXPECT_FALSE(domain->try_immediate(waiting, false));
  auto oversized = std::move(waiting).into_request();
  EXPECT_EQ(
      domain->prepare_submit(std::move(oversized), rejected.target(), error)
          .value,
      0u);
  EXPECT_EQ(error, EAGAIN);
  EXPECT_EQ(rejected.count, 0u);
  ASSERT_EQ(::send(first.fd(1), "a", 1, 0), 1);
  for (unsigned i = 0; i < 20 && !received.count; ++i)
    (void)engine.driver().wait_and_drive(10);
  ASSERT_EQ(received.count, 1u);
  EXPECT_EQ(received.result, 1);
  EXPECT_EQ(byte, 'a');
  auto again = read_request(other, blocked);
  EXPECT_FALSE(domain->try_immediate(again, false));
  auto reused = std::move(again).into_request();
  const auto current =
      domain->prepare_submit(std::move(reused), next.target(), error);
  ASSERT_NE(current.value, 0u);
  EXPECT_EQ(static_cast<std::uint32_t>(current.value),
            static_cast<std::uint32_t>(old.value));
  EXPECT_GT(current.value, old.value);
  domain->request_cancel(old); // 旧代际不能取消新的真实等待。
  ASSERT_EQ(::send(second.fd(1), "b", 1, 0), 1);
  for (unsigned i = 0; i < 20 && !next.count; ++i)
    (void)engine.driver().wait_and_drive(10);
  EXPECT_EQ(next.count, 1u);
  EXPECT_EQ(next.result, 1);
  EXPECT_EQ(blocked, 'b');
}

TEST(CompactScalarContract,
     ExternalXvalueMayBeDestroyedWhileRealReceiveIsPending) {
  using namespace compact_scalar_test;
  descriptor_pair pair;
  char byte{};
  faio::io::io_engine engine{faio_test::engine_config()};
  manual_scheduler scheduler;
  const auto domain = engine.context().domain();
  auto resource = domain->adopt(pair.fd(0), false);
  std::weak_ptr<resource_state> witness = resource;
  auto external = std::make_unique<faio::io::detail::Recv>(std::move(resource),
                                                           &byte, 1, 0);
  auto pending = consume_external(*external);
  auto handle = pending.take();
  handle.promise().context.scheduler = faio::scheduler_ref{scheduler};
  faio::io::io_engine::binding io_binding{engine};
  faio::detail::execution_thread_binding execution{
      faio::scheduler_ref{scheduler}, {}, scheduler, 0};
  faio::detail::execution_thread_guard installed{execution};
  frame_owner frame{handle, engine, scheduler};
  scheduler.enqueue(handle);
  scheduler.pump();
  ASSERT_FALSE(handle.done()); // 没有发送数据，RECV 必须真实挂起。
  external.reset(); // 挂起后才销毁 xvalue 原对象，不能借用它的 request/consumer
                    // 地址。
  EXPECT_FALSE(witness.expired());
  ASSERT_EQ(::send(pair.fd(1), "x", 1, 0), 1);
  for (unsigned i = 0; i < 20 && !handle.done(); ++i) {
    (void)engine.driver().wait_and_drive(10);
    scheduler.pump();
  }
  ASSERT_TRUE(handle.done());
  auto result = handle.promise().take_result();
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 1u);
  EXPECT_EQ(byte, 'x');
  engine.shutdown(); // 完成发布者与稳定 request 排空后检查不残留强租约。
  EXPECT_TRUE(witness.expired());
}

TEST(CompactScalarContract,
     RealAwaiterPoolFailureDoesNotPublishOrKeepDirectionBusy) {
  using namespace compact_scalar_test;
  descriptor_pair first, second;
  char one{}, two{};
  auto config = faio_test::engine_config();
  config.max_operations = 1;
  faio::io::io_engine engine{config};
  manual_scheduler scheduler;
  auto domain = engine.context().domain();
  auto first_resource = domain->adopt(first.fd(0), false);
  std::weak_ptr<resource_state> source;
  faio::io::io_engine::binding io_binding{engine};
  faio::detail::execution_thread_binding execution{
      faio::scheduler_ref{scheduler}, {}, scheduler, 0};
  faio::detail::execution_thread_guard installed{execution};
  {
    auto second_resource = domain->adopt(second.fd(0), false);
    source = second_resource;
    auto second_operation =
        faio::io::recv(std::move(second_resource), &two, 1, 0).set_timeout(1s);
    EXPECT_FALSE(second_resource); // 外部最后强引用已经转入 awaiter，domain
                                   // 只保存 weak 表。
    auto initial = consume_owned(
        faio::io::recv(first_resource, &one, 1, 0).set_timeout(1s));
    auto initial_handle = initial.take();
    initial_handle.promise().context.scheduler = faio::scheduler_ref{scheduler};
    frame_owner pending{initial_handle, engine, scheduler};
    scheduler.enqueue(initial_handle);
    scheduler.pump();
    ASSERT_FALSE(initial_handle.done());
    auto rejected = consume_borrowed(second_operation);
    auto rejected_handle = rejected.take();
    rejected_handle.promise().context.scheduler =
        faio::scheduler_ref{scheduler};
    frame_owner failure{rejected_handle, engine, scheduler};
    scheduler.enqueue(rejected_handle);
    scheduler.pump();
    ASSERT_TRUE(rejected_handle.done());
    const auto no_capacity = rejected_handle.promise().take_result();
    ASSERT_FALSE(no_capacity);
    EXPECT_EQ(no_capacity.error().value(), EAGAIN);
    EXPECT_FALSE(
        source.expired()); // domain 只持 weak；拒绝后强租约必须仍在原命名
                           // awaiter。
    ASSERT_EQ(::send(first.fd(1), "1", 1, 0), 1);
    for (unsigned i = 0; i < 20 && !initial_handle.done(); ++i) {
      (void)engine.driver().wait_and_drive(10);
      scheduler.pump();
    }
    ASSERT_TRUE(initial_handle.done());
    EXPECT_TRUE(initial_handle.promise().take_result());
    EXPECT_EQ(one, '1');
    // 容量失败不能留下 consumer、reader 或 sticky
    // cancel；同方向新操作必须可等待。
    auto following = consume_borrowed(
        second_operation); // 同一未接受 awaiter 保持旧桥的可重试参数。
    auto following_handle = following.take();
    following_handle.promise().context.scheduler =
        faio::scheduler_ref{scheduler};
    frame_owner reusable{following_handle, engine, scheduler};
    scheduler.enqueue(following_handle);
    scheduler.pump();
    ASSERT_FALSE(following_handle.done());
    ASSERT_EQ(::send(second.fd(1), "2", 1, 0), 1);
    for (unsigned i = 0; i < 20 && !following_handle.done(); ++i) {
      (void)engine.driver().wait_and_drive(10);
      scheduler.pump();
    }
    ASSERT_TRUE(following_handle.done());
    const auto received = following_handle.promise().take_result();
    ASSERT_TRUE(received);
    EXPECT_EQ(*received, 1u);
    EXPECT_EQ(two, '2');
  } // 手工任务帧先销毁，随后命名 awaiter
    // 结束；不依赖错误结果仍保有外部资源副本。
  engine
      .shutdown(); // 原生最终 CQE、publisher 与关闭责任全部排空后才检查无残留。
  EXPECT_TRUE(source.expired());
}

TEST(CompactScalarContract,
     TimedExternalXvalueSourceMayBeDestroyedDuringRealPendingReceive) {
  using namespace compact_scalar_test;
  descriptor_pair pair;
  char byte{};
  faio::io::io_engine engine{faio_test::engine_config()};
  manual_scheduler scheduler;
  auto resource = engine.context().domain()->adopt(pair.fd(0), false);
  std::weak_ptr<resource_state> witness = resource;
  auto external = std::make_unique<timed_receive>(
      faio::time::timeout_at(faio::io::recv(std::move(resource), &byte, 1, 0),
                             std::chrono::steady_clock::now() + 1s));
  auto pending = consume_external_timeout(*external);
  auto handle = pending.take();
  handle.promise().context.scheduler = faio::scheduler_ref{scheduler};
  faio::io::io_engine::binding io_binding{engine};
  faio::detail::execution_thread_binding execution{
      faio::scheduler_ref{scheduler}, {}, scheduler, 0};
  faio::detail::execution_thread_guard installed{execution};
  frame_owner frame{handle, engine, scheduler};
  scheduler.enqueue(handle);
  scheduler.pump();
  ASSERT_FALSE(handle.done()); // 没有 payload，必须真实挂起以后才销毁外部源。
  external.reset();
  EXPECT_FALSE(witness.expired());
  ASSERT_EQ(::send(pair.fd(1), "t", 1, 0), 1);
  for (unsigned i = 0; i < 20 && !handle.done(); ++i) {
    (void)engine.driver().wait_and_drive(10);
    scheduler.pump();
  }
  ASSERT_TRUE(handle.done());
  const auto result = handle.promise().take_result();
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 1u);
  EXPECT_EQ(byte, 't');
  engine.shutdown();
  EXPECT_TRUE(witness.expired());
}

class CompactScalarRuntimeContract
    : public testing::TestWithParam<faio::runtime::mode> {
protected:
  auto config() const {
    return faio_test::config_builder()
        .set_mode(GetParam())
        .set_num_workers(4)
        .build();
  }
};
TEST_P(CompactScalarRuntimeContract,
       ConnectedUdpZeroDatagramConsumesBeforeNextPayload) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(
      runtime.block_on(compact_scalar_test::udp_zero_and_next_payload()));
}
TEST_P(CompactScalarRuntimeContract,
       RealTcpLargeScalarTransfersPreservePeekAndFlags) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(compact_scalar_test::tcp_large_scalar_flags()));
}
TEST_P(CompactScalarRuntimeContract,
       DeferredSendCapturesResourceBeforeWrapperDestruction) {
  auto endpoints = faio_test::take(
      faio::net::unix::UnixStream::pair(faio::io::io_context{}));
  std::weak_ptr<faio::io::detail::resource_state> witness =
      endpoints.first.resource();
  const std::array<char, 4> bytes{'o', 'w', 'n', '!'};
  auto operation = [&] {
    auto original = std::move(endpoints.first);
    return faio::io::send(original.resource(), bytes.data(), bytes.size(), 0);
  }(); // 原包装已经析构；请求必须在调用时捕获，而不能等 await 再借用 this。
  EXPECT_FALSE(witness.expired());
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(compact_scalar_test::complete_deferred_send(
      std::move(operation), endpoints.second)));
  runtime.stop(faio::io::shutdown_policy::drain);
  EXPECT_TRUE(witness.expired());
}
TEST_P(CompactScalarRuntimeContract,
       TimeoutHelpersForwardCompactFullAndCompatWrappers) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(
      compact_scalar_test::timeout_helpers_transfer_compact_and_full()));
}
TEST_P(CompactScalarRuntimeContract,
       TimeoutHelpersDeliverRealDeadlineAndReleaseDirections) {
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(
      compact_scalar_test::timeout_helpers_deadline_and_reuse()));
}
TEST_P(CompactScalarRuntimeContract,
       TimeoutWrappersOwnResourcesBeforeDestroyedUnboundWrappers) {
  using namespace compact_scalar_test;
  auto send_pair =
      take(faio::net::unix::UnixStream::pair(faio::io::io_context{}));
  auto recv_pair =
      take(faio::net::unix::UnixStream::pair(faio::io::io_context{}));
  std::weak_ptr<resource_state> send_source = send_pair.first.resource();
  std::weak_ptr<resource_state> recv_source = recv_pair.first.resource();
  const char payload = 's';
  char received{};
  auto sending = [&] {
    auto original = std::move(send_pair.first);
    return timed_send{faio::time::timeout(
        faio::io::send(original.resource(), &payload, 1, 0), 2s)};
  }(); // 调用时捕获后立即销毁原 wrapper，只剩 Timeout 内部拥有的资源。
  auto receiving = [&] {
    auto original = std::move(recv_pair.first);
    return timed_receive{faio::time::timeout_at(
        faio::io::recv(original.resource(), &received, 1, 0),
        std::chrono::steady_clock::now() + 2s)};
  }();
  EXPECT_FALSE(send_source.expired());
  EXPECT_FALSE(recv_source.expired());
  faio_test::runtime_context runtime{config()};
  EXPECT_TRUE(runtime.block_on(
      complete_deferred_timed_send(std::move(sending), send_pair.second)));
  EXPECT_TRUE(runtime.block_on(
      complete_deferred_timed_recv(std::move(receiving), recv_pair.second)));
  EXPECT_EQ(received, 'r');
  runtime.stop(faio::io::shutdown_policy::
                   drain); // 排空 stable/publisher/原生关闭后观察无残留。
  EXPECT_TRUE(send_source.expired());
  EXPECT_TRUE(recv_source.expired());
}
INSTANTIATE_TEST_SUITE_P(RuntimeModes, CompactScalarRuntimeContract,
                         testing::Values(faio::runtime::mode::current_thread,
                                         faio::runtime::mode::multi_thread));
