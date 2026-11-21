/**
 * @file test_net.cpp
 * @brief 地址解析、TCP 回显和 UDP 数据报的单元测试。
 */
#include "test_helpers.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <thread>

namespace {
/** @brief 接受一个连接并回传四字节消息。 */
faio::task<void> echo(faio::net::TcpListener& listener) {
  auto accepted = faio_test::take(co_await listener.accept());
  auto stream = std::move(accepted.first);
  std::array<char, 4> bytes{};
  faio_test::take(co_await stream.read_exact(bytes));
  faio_test::take(co_await stream.write_all(bytes));
  faio_test::take(co_await stream.shutdown(faio::io::ShutdownBehavior::Write));
}

/** @brief 通过本地 TCP 连接验证完整读写与 EOF。 */
faio::task<bool> tcp_roundtrip() {
  auto listener = faio_test::take(
      faio::net::TcpListener::bind(faio_test::take(faio::net::address::parse("127.0.0.1", 0))));
  auto server = faio::spawn(echo(listener));
  auto client = faio_test::take(
      co_await faio::net::TcpStream::connect(faio_test::take(listener.local_addr())));
  faio_test::take(client.set_nodelay(true));
  const std::array<char, 4> payload{'f', 'a', 'i', 'o'};
  faio_test::take(co_await client.write_all(payload));
  std::array<char, 4> bytes{};
  faio_test::take(co_await client.read_exact(bytes));
  char tail{};
  const auto eof = faio_test::take(co_await client.read(std::span<char>{&tail, 1}));
  co_await server;
  co_return bytes == payload&& eof == 0 && faio_test::take(client.nodelay());
}

/** @brief 连续回传单字节消息，用较大的缓冲区接收，确保发生短读。 */
faio::task<void> short_read_echo(faio::net::TcpListener& listener) {
  auto accepted = faio_test::take(co_await listener.accept());
  auto stream = std::move(accepted.first);
  std::array<char, 32> bytes{};
  for (unsigned round = 0; round != 16; ++round) {
    const auto count =
        faio_test::take(co_await stream.read(bytes).set_timeout(std::chrono::seconds{2}));
    if (count != 1)
      throw std::runtime_error("短读回显长度错误");
    faio_test::take(co_await stream.write_all(std::span<const char>{bytes}.first(count)));
  }
  faio_test::take(co_await stream.shutdown(faio::io::ShutdownBehavior::Write));
}

/** @brief 短读后的下一批数据、窥视及对端半关闭均能正常完成。 */
faio::task<bool> stream_short_reads() {
  auto listener = faio_test::take(
      faio::net::TcpListener::bind(faio_test::take(faio::net::address::parse("127.0.0.1", 0))));
  auto server = faio::spawn(short_read_echo(listener));
  auto client = faio_test::take(
      co_await faio::net::TcpStream::connect(faio_test::take(listener.local_addr())));
  std::array<char, 32> bytes{};
  for (unsigned round = 0; round != 16; ++round) {
    const char value = static_cast<char>('a' + round);
    faio_test::take(co_await client.write_all(std::span<const char>{&value, 1}));
    if (round % 2 == 0) {
      const auto peeked =
          faio_test::take(co_await client.peek(bytes).set_timeout(std::chrono::seconds{2}));
      if (peeked != 1 || bytes[0] != value)
        co_return false;
    }
    const auto count =
        faio_test::take(co_await client.read(bytes).set_timeout(std::chrono::seconds{2}));
    if (count != 1 || bytes[0] != value)
      co_return false;
  }
  const auto eof =
      faio_test::take(co_await client.read(bytes).set_timeout(std::chrono::seconds{2}));
  co_await server;
  co_return eof == 0;
}

/** @brief 通知父任务后等待下一字节，等待期间可由外部线程取消。 */
faio::task<faio::expected<std::size_t>> read_until_cancelled(faio::net::TcpStream& stream,
                                                             faio::sync::latch& started) {
  std::array<char, 1> bytes{};
  started.count_down();
  co_return co_await stream.read(bytes).set_timeout(std::chrono::seconds{2});
}

/** @brief 挂起的读取在取消或超时后释放读取权，后续读取仍能接收完整数据。 */
faio::task<bool> read_after_cancel_and_timeout() {
  auto listener = faio_test::take(
      faio::net::TcpListener::bind(faio_test::take(faio::net::address::parse("127.0.0.1", 0))));
  auto client = faio_test::take(
      co_await faio::net::TcpStream::connect(faio_test::take(listener.local_addr())));
  auto accepted = faio_test::take(co_await listener.accept());
  auto& stream = accepted.first;

  faio::sync::latch started{1};
  auto pending = faio::spawn(read_until_cancelled(stream, started));
  co_await started.wait();
  std::jthread canceller([&] { pending.request_stop(); });
  canceller.join();
  const auto cancelled = co_await pending;
  if (cancelled || cancelled.error().value() != ECANCELED)
    co_return false;

  std::array<char, 1> bytes{};
  const auto timeout = co_await stream.read(bytes).set_timeout(std::chrono::milliseconds{5});
  if (timeout || timeout.error().value() != ETIMEDOUT)
    co_return false;

  const std::array<char, 1> payload{'x'};
  faio_test::take(co_await client.write_all(payload));
  const auto count =
      faio_test::take(co_await stream.read(bytes).set_timeout(std::chrono::seconds{2}));
  co_return count == payload.size() && bytes == payload;
}

/** @brief UDP 保留数据报边界，包括空数据报。 */
faio::task<bool> udp_roundtrip() {
  const auto loopback = faio_test::take(faio::net::address::parse("127.0.0.1", 0));
  auto sender = faio_test::take(faio::net::UdpDatagram::bind(loopback));
  auto receiver = faio_test::take(faio::net::UdpDatagram::bind(loopback));
  const auto destination = faio_test::take(receiver.local_addr());
  const std::array<char, 4> payload{'U', 'D', 'P', '!'};
  faio_test::take(co_await sender.send_to(payload, destination));
  std::array<char, 16> bytes{};
  const auto received = faio_test::take(co_await receiver.recv_from(bytes));
  if (received.first != payload.size()
      || !std::equal(payload.begin(), payload.end(), bytes.begin()))
    co_return false;
  faio_test::take(co_await sender.send_to(std::span<const char>{}, destination));
  const auto empty = faio_test::take(co_await receiver.recv_from(bytes));
  co_return empty.first == 0;
}
}  // namespace

/** @brief 数字地址支持 IPv4、IPv6，并拒绝主机名。 */
TEST(Net, Addresses) {
  const auto ipv4 = faio::net::address::parse("127.0.0.1", 8080);
  ASSERT_TRUE(ipv4);
  EXPECT_TRUE(ipv4->is_ipv4());
  EXPECT_EQ(ipv4->port(), 8080);
  const auto ipv6 = faio::net::address::parse("::1", 9000);
  ASSERT_TRUE(ipv6);
  EXPECT_TRUE(ipv6->is_ipv6());
  EXPECT_EQ(ipv6->port(), 9000);
  EXPECT_FALSE(faio::net::address::parse("localhost", 80));
}

/** @brief TCP 连接完成回显与写方向关闭。 */
TEST(Net, Tcp) {
  for (const auto workers : {1u, 2u}) {
    const auto config = faio::ConfigBuilder{}.set_num_workers(workers).build();
    faio::runtime::detail::runtime_context context{config};
    EXPECT_TRUE(context.block_on(tcp_roundtrip()));
  }
}

/** @brief UDP 连接传输普通数据报与空数据报。 */
TEST(Net, Udp) {
  faio::runtime::detail::runtime_context context{faio_test::config()};
  EXPECT_TRUE(context.block_on(udp_roundtrip()));
}

/** @brief 单个或多个 worker 下的连续短读均保留窥视、数据与 EOF 语义。 */
TEST(Net, StreamShortReads) {
  for (const auto mode : {faio::runtime::mode::current_thread, faio::runtime::mode::multi_thread}) {
    faio::runtime::detail::runtime_context context{faio_test::config(mode)};
    EXPECT_TRUE(context.block_on(stream_short_reads()));
  }
}

/** @brief 单线程 TCP 读取消及超时后仍可继续接收。 */
TEST(Net, CurrentThreadReadAfterCancelAndTimeout) {
  faio::runtime::detail::runtime_context context{
      faio_test::config(faio::runtime::mode::current_thread)};
  EXPECT_TRUE(context.block_on(read_after_cancel_and_timeout()));
}
