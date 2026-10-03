#include "backend_test_support.hpp"
/**
 * @file test_network_contract.cpp
 * @brief 公共 TCP/UDP API 的行为契约；在 uring/epoll/kqueue 和两种 runtime
 * 模式下执行。
 * @details 回环测试使用临时端口；所有协程与资源按具名 task 生命周期排空。
 */
#include "faio/faio.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/uio.h>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using context = faio::runtime::detail::runtime_context;

/** @brief 将 expected 失败传播到测试主线程，避免 coroutine 内的 ASSERT
 * 提早返回。 */
template <class T> T take(faio::expected<T> result) {
  if (!result)
    throw std::runtime_error(std::string(result.error().message()));
  return std::move(*result);
}
void check(faio::expected<void> result) {
  if (!result)
    throw std::runtime_error(std::string(result.error().message()));
}

faio::task<void> echo_vectored(faio::net::TcpListener &listener) {
  auto accepted = take(co_await listener.accept());
  auto stream = std::move(accepted.first);
  std::array<char, 8> input{};
  check(co_await stream.read_exact(input));
  const auto written =
      take(co_await stream.write_v(std::span<const char>{input}.first(4),
                                   std::span<const char>{input}.subspan(4)));
  if (written < input.size())
    check(co_await stream.write_all(
        std::span<const char>{input}.subspan(written)));
  check(co_await stream.shutdown(faio::io::ShutdownBehavior::Write));
}

/** @brief peek 不消费、vectored 部分 IO、空读与 EOF 分离。 */
faio::task<bool> tcp_vectored_and_eof() {
  auto listener = take(faio::net::TcpListener::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  const auto address = take(listener.local_addr());
  auto peer = faio::spawn(echo_vectored(listener));
  auto client = take(co_await faio::net::TcpStream::connect(address));
  check(client.set_nodelay(true));
  if (!take(client.nodelay()))
    throw std::runtime_error("TCP_NODELAY not set");
  // 空 stream 请求在没有数据时也立即完成；不能依赖 EOF 或内核 recv(null,0)。
  if (take(co_await client.read(std::span<char>{}).set_timeout(20ms)) != 0 ||
      take(co_await client.read_v(std::span<char>{}, std::span<char>{})
               .set_timeout(20ms)) != 0)
    co_return false;
  const std::array<char, 8> payload{'f', 'a', 'i', 'o', 'T', 'C', 'P', '!'};
  check(co_await client.write_all(payload));
  char first{};
  if (take(co_await client.read(std::span<char>{}).set_timeout(20ms)) != 0)
    co_return false;
  if (take(co_await client.peek(std::span<char>{&first, 1})) != 1 ||
      first != payload[0])
    throw std::runtime_error("peek consumed or corrupted bytes");
  std::array<char, 4> left{}, right{};
  const auto read = take(co_await client.read_v(left, right));
  std::array<char, 8> combined{};
  std::memcpy(combined.data(), left.data(), 4);
  std::memcpy(combined.data() + 4, right.data(), 4);
  if (read < combined.size())
    check(co_await client.read_exact(std::span<char>{combined}.subspan(read)));
  if (combined != payload)
    throw std::runtime_error("vectored echo differs");
  if (take(co_await client.read(std::span<char>{})) != 0)
    throw std::runtime_error("empty read failed");
  if (take(co_await client.read(std::span<char>{&first, 1})) != 0)
    throw std::runtime_error("EOF not reported");
  co_await peer;
  co_return true;
}

/**
 * @brief 异步 connect 的 SOCKET 和批量接受的每个连接均来自原生完成。
 * @details 三个客户端先完成握手，再接受最多四条；多余容量应立即停止，
 * 不能等待第四条不存在的连接。显式 context 使证据归属于同一 IO domain。
 */
faio::task<bool> tcp_native_connect_and_partial_accept_batch() {
  auto owner = faio::io::io_context::current();
  const bool native = owner.domain()->capabilities().native_filesystem;
  if (native && (owner.blocking().started_threads() != 0 ||
                 owner.resolver().started_threads() != 0 ||
                 owner.cleanup().started_threads() != 0))
    co_return false;
  auto listener = take(faio::net::TcpListener::bind(
      owner, take(faio::net::address::parse("127.0.0.1", 0))));
  const auto address = take(listener.local_addr());
  const auto invalid = co_await listener.accept_many(0, owner);
  if (invalid || invalid.error().value() != EINVAL)
    co_return false;
  std::vector<faio::net::TcpStream> clients;
  clients.reserve(3);
  std::set<std::uint16_t> peer_ports;
  for (std::size_t index = 0; index < 3; ++index) {
    const auto before = owner.statistics();
    clients.push_back(
        take(co_await faio::net::TcpStream::connect(owner, address)));
    peer_ports.insert(take(clients.back().local_addr()).port());
    const auto after = owner.statistics();
    if (owner.domain()->supports_native(
            faio::io::detail::operation_kind::socket) &&
        (after.native_submitted < before.native_submitted + 2 ||
         after.native_completed < before.native_completed + 2 ||
         after.native_flushed < before.native_flushed + 2))
      co_return false; // SOCKET 与 CONNECT 均需实际提交；同步 socket 不可代替。
  }
  const auto before_accept = owner.statistics();
  std::vector<std::pair<faio::net::TcpStream, faio::net::address>> accepted;
  accepted.reserve(clients.size());
  while (accepted.size() < clients.size()) {
    auto pending = faio::spawn(listener.accept_many(4, owner));
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!pending.done() && std::chrono::steady_clock::now() < deadline)
      co_await faio::time::sleep(1ms);
    if (!pending.done()) {
      pending.request_stop();
      (void)co_await pending;
      co_return false; // 正确的批量余量使用 DONTWAIT，不会无限等待。
    }
    auto batch = take(co_await pending);
    if (batch.empty() || accepted.size() + batch.size() > clients.size())
      co_return false;
    for (auto &connection : batch) {
      if (connection.first.context().domain() != owner.domain() ||
          peer_ports.erase(connection.second.port()) != 1)
        co_return false;
      accepted.push_back(std::move(connection));
    }
  }
  const auto after_accept = owner.statistics();
  if (owner.domain()->supports_native(
          faio::io::detail::operation_kind::accept) &&
      (after_accept.native_submitted < before_accept.native_submitted + 3 ||
       after_accept.native_completed < before_accept.native_completed + 3 ||
       after_accept.native_flushed < before_accept.native_flushed + 3))
    co_return false; // 三个已接受 fd 都来自 ACCEPT CQE。
  for (auto &connection : accepted) {
    check(co_await connection.first.write_all(std::span<const char>{"B", 1}));
    check(co_await connection.first.close());
  }
  for (auto &client : clients) {
    char byte{};
    check(co_await client.read_exact(std::span<char>{&byte, 1}));
    if (byte != 'B' ||
        take(co_await client.read(std::span<char>{&byte, 1})) != 0)
      co_return false;
    check(co_await client.close());
  }
  check(co_await listener.close());
  co_return peer_ports.empty() &&
      (!native || (owner.blocking().started_threads() == 0 &&
                   owner.resolver().started_threads() == 0 &&
                   owner.cleanup().started_threads() == 0));
}

faio::task<void> send_large_payload(faio::net::TcpListener &listener,
                                    std::size_t size) {
  auto accepted = take(co_await listener.accept());
  auto stream = std::move(accepted.first);
  std::vector<char> payload(size, 'b');
  check(co_await stream.write_all(payload));
}

/** @brief 收包侧完整校验；发送方每次完成后会立刻覆盖已经归还的借用buffer。 */
faio::task<void> verify_zero_copy_peer(faio::net::TcpListener &listener,
                                       std::size_t size) {
  auto accepted = take(co_await listener.accept());
  std::vector<char> bytes(size);
  check(co_await accepted.first.read_exact(bytes));
  for (std::size_t index = 0; index < bytes.size(); ++index)
    if (bytes[index] != static_cast<char>('a' + index % 26))
      throw std::runtime_error(
          "zero-copy borrowed payload changed before delivery");
  check(co_await accepted.first.write_all(std::span<const char>{"ack", 3}));
}

/** @brief uring原生ZC需要真实NOTIF；reactor
 * fallback保留同一短写和借用归还语义。 */
faio::task<bool> tcp_zero_copy_releases_borrowed_buffer() {
  auto listener = take(faio::net::TcpListener::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  constexpr std::size_t size = 256 * 1024;
  auto receiving = faio::spawn(verify_zero_copy_peer(listener, size));
  auto stream =
      take(co_await faio::net::TcpStream::connect(take(listener.local_addr())));
  check(stream.set_send_buffer_size(
      16 * 1024)); // 有限发送窗口产生真实短写/挂起，循环保留余量。
  if (take(co_await stream.write_zc(std::span<const char>{})
               .set_timeout(100ms)) != 0)
    throw std::runtime_error("empty zero-copy stream write did not finish");
  const auto context = stream.context();
  const auto before = context.statistics();
  std::vector<char> payload(size);
  for (std::size_t index = 0; index < payload.size(); ++index)
    payload[index] = static_cast<char>('a' + index % 26);
  std::size_t completed{};
  while (completed < payload.size()) {
    const auto written =
        take(co_await stream
                 .write_zc(std::span<const char>{payload}.subspan(completed))
                 .set_timeout(5s));
    if (!written || written > payload.size() - completed)
      throw std::runtime_error("zero-copy write made invalid progress");
    std::fill_n(payload.data() + completed, written, 'x');
    completed +=
        written; // await返回前该前缀必须已解除内核引用，立即复用是合法的。
  }
  const auto after = context.statistics();
  std::array<char, 3> acknowledgement{};
  check(co_await stream.read_exact(acknowledgement));
  co_await receiving;
  if (acknowledgement != std::array<char, 3>{'a', 'c', 'k'})
    co_return false;
  if (context.domain()->capabilities().zero_copy)
    co_return after.native_completed >
        before.native_completed &&after.buffer_notifications >
        before.buffer_notifications;
  co_return after.buffer_notifications == before.buffer_notifications;
}

/** @brief 限制 socket buffer 后的大包写入必须处理短写与慢读背压。 */
faio::task<bool> tcp_backpressure() {
  auto listener = take(faio::net::TcpListener::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  constexpr std::size_t size = 1024 * 1024;
  auto peer = faio::spawn(send_large_payload(listener, size));
  auto client =
      take(co_await faio::net::TcpStream::connect(take(listener.local_addr())));
  co_await faio::time::sleep(2ms);
  std::array<char, 4096> buffer{};
  std::size_t total = 0;
  while (total < size) {
    const auto n = take(co_await client.read(buffer));
    if (!n)
      throw std::runtime_error("early EOF during backpressure transfer");
    for (std::size_t i = 0; i < n; ++i)
      if (buffer[i] != 'b')
        throw std::runtime_error("large transfer corruption");
    total += n;
  }
  co_await peer;
  co_return total == size;
}

/** @brief UDP 零长度包是有效数据报，不能解释为 stream EOF。 */
faio::task<bool> udp_empty_and_full() {
  auto first = take(faio::net::UdpDatagram::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  auto second = take(faio::net::UdpDatagram::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  const auto destination = take(second.local_addr());
  if (take(co_await first.send_to(std::span<const char>{}, destination)) != 0)
    throw std::runtime_error("empty datagram send");
  std::array<char, 64> buffer{};
  auto empty = take(co_await second.recv_from(buffer));
  if (empty.first != 0 ||
      empty.second.port() != take(first.local_addr()).port())
    throw std::runtime_error("empty datagram receive");
  const std::array<char, 4> payload{'U', 'D', 'P', '!'};
  if (take(co_await first.send_to(payload, destination)) != payload.size())
    throw std::runtime_error("datagram send");
  auto received = take(co_await second.recv_from(buffer));
  if (received.first != payload.size() ||
      !std::equal(payload.begin(), payload.end(), buffer.begin()))
    co_return false;
  // 与 stream 空读不同，零容量 UDP buffer 会等待并消费一个真实数据报。
  if (take(co_await first.send_to(payload, destination)) != payload.size())
    co_return false;
  if (take(co_await second.recv_from(std::span<char>{}).set_timeout(20ms))
          .first != 0)
    throw std::runtime_error(
        "zero-capacity UDP receive returned a nonzero copied length");
  const auto consumed = co_await second.recv_from(buffer).set_timeout(3ms);
  if (consumed)
    throw std::runtime_error(
        "zero-capacity UDP receive left the datagram queued");
  co_return consumed.error().value() == ETIMEDOUT;
}

/** @brief 同一资源连接后仍能保留创建 domain 与 peer 地址。 */
faio::task<bool> udp_connected_and_peek() {
  auto first = take(faio::net::UdpDatagram::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  auto second = take(faio::net::UdpDatagram::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  check(co_await first.connect(take(second.local_addr())));
  check(co_await second.connect(take(first.local_addr())));
  const std::array<char, 3> payload{'x', 'y', 'z'};
  if (take(co_await first.send(payload)) != payload.size())
    throw std::runtime_error("connected send");
  std::array<char, 8> buffer{};
  if (take(co_await second.peek(buffer)) != payload.size())
    throw std::runtime_error("UDP peek");
  const auto n = take(co_await second.recv(buffer));
  co_return n == payload.size() &&
      std::equal(payload.begin(), payload.end(), buffer.begin()) &&
      take(second.peer_addr()).port() == take(first.local_addr()).port();
}

/** @brief 就绪观察不消费数据，try_* 的 EAGAIN 可在下一次 ready 后重试。 */
faio::task<bool> tcp_config_ready_native_and_owned() {
  auto context = faio::io::io_context::current();
  auto socket = take(faio::net::TcpSocket::new_v4(context));
  check(socket.set_reuseaddr(true));
  check(socket.set_reuseport(true));
  check(socket.set_keepalive(true));
  check(socket.set_linger(std::nullopt));
  check(socket.set_recv_buffer_size(8192));
  check(socket.set_send_buffer_size(8192));
  check(socket.bind(take(faio::net::address::parse("127.0.0.1", 0))));
  auto listener = take(std::move(socket).listen(128));
  auto peer = faio::spawn(echo_vectored(listener));
  auto connecting = take(faio::net::TcpSocket::new_v4(context));
  check(connecting.set_nodelay(true));
  check(connecting.set_ttl(42));
  auto client =
      take(co_await std::move(connecting).connect(take(listener.local_addr())));
  if (!take(client.nodelay()) || take(client.ttl()) != 42 ||
      take(client.take_error()))
    co_return false;
  std::array<char, 8> output{};
  auto empty = client.try_read(output);
  if (empty ||
      (empty.error().value() != EAGAIN && empty.error().value() != EWOULDBLOCK))
    co_return false;
  if (!take(co_await client.writable()).is_writable())
    co_return false;
  auto written = take(co_await client.write(
      faio::io::io_buffer::copy(std::string_view{"advanced"})));
  if (written.bytes != 8 || written.buffer.size() != 8)
    co_return false;
  if (!take(co_await client.readable()).is_readable())
    co_return false;
  if (take(client.try_peek(output)) != 8 ||
      std::string_view(output.data(), 8) != "advanced")
    co_return false;
  faio::io::read_buf read_buffer{output};
  if (take(client.try_read_buf(read_buffer)) != 8 || read_buffer.size() != 8)
    co_return false;
  co_await peer;
  auto native = take(client.into_native());
  auto imported =
      take(faio::net::TcpStream::from_native(context, std::move(native)));
  if (take(co_await imported.read(output)) != 0)
    co_return false;
  check(co_await imported.close());
  check(co_await listener.close());
  co_return true;
}

/** @brief sendmsg 的 iovec 顺序及 recvmsg 的截断必须保留数据报边界。 */
faio::task<bool> udp_messages_and_batch() {
  auto owner = faio::io::io_context::current();
  auto first = take(faio::net::UdpSocket::bind(
      owner, take(faio::net::address::parse("127.0.0.1", 0))));
  auto second = take(faio::net::UdpSocket::bind(
      owner, take(faio::net::address::parse("127.0.0.1", 0))));
  const auto destination = take(second.local_addr());
  check(first.set_broadcast(true));
  check(first.set_ttl(32));
  check(first.set_multicast_loop_v4(false));
  check(first.set_multicast_ttl_v4(7));
  if (!take(first.broadcast()) || take(first.ttl()) != 32 ||
      take(first.multicast_loop_v4()) || take(first.multicast_ttl_v4()) != 7)
    co_return false;
  std::array<char, 8> empty_buffer{};
  auto no_packet = second.try_recv_from(empty_buffer);
  if (no_packet || (no_packet.error().value() != EAGAIN &&
                    no_packet.error().value() != EWOULDBLOCK))
    co_return false;
  std::array<char, 4> left{'s', 'e', 'n', 'd'}, right{'m', 's', 'g', '!'};
  const std::array<iovec, 2> vectors{
      {{left.data(), left.size()}, {right.data(), right.size()}}};
  if (take(co_await first.send_message(vectors, destination)) != 8)
    co_return false;
  std::array<char, 3> short_buffer{};
  std::array<std::byte, 128> control{};
  const auto message =
      take(co_await second.recv_message(short_buffer, control));
  if (message.copied_bytes != 3 || !message.truncated ||
      std::string_view(short_buffer.data(), 3) != "sen")
    co_return false;
  if (message.peer.port() != take(first.local_addr()).port())
    co_return false;
  const std::array<faio::net::detail::DatagramSend<faio::net::address>, 3>
      messages{{{std::span<const char>{left}, destination},
                {std::span<const char>{right}, destination},
                {{}, destination}}};
  if (take(co_await first.send_many(messages)) != 3)
    co_return false;
  std::array<std::array<char, 8>, 3> storage{};
  std::array<std::span<char>, 3> buffers{storage[0], storage[1], storage[2]};
  std::vector<faio::net::DatagramMessage> received;
  const auto before_receive = owner.statistics();
  while (received.size() < buffers.size()) {
    auto batch = take(
        co_await second.recv_many(std::span{buffers}.subspan(received.size())));
    if (batch.empty())
      co_return false;
    received.insert(received.end(), batch.begin(), batch.end());
  }
  const auto after_receive = owner.statistics();
  if (owner.domain()->supports_native(
          faio::io::detail::operation_kind::recvmsg) &&
      (after_receive.native_completed < before_receive.native_completed + 3 ||
       after_receive.native_submitted < before_receive.native_submitted + 3 ||
       after_receive.native_flushed < before_receive.native_flushed + 3))
    co_return false; // 第二、第三包同样来自 RECVMSG CQE，不能借用同步 recvmsg。
  if (received.size() != 3 || received[0].copied_bytes != 4 ||
      received[1].copied_bytes != 4 || received[2].copied_bytes != 0)
    co_return false;
  if (std::string_view(storage[0].data(), 4) != "send" ||
      std::string_view(storage[1].data(), 4) != "msg!")
    co_return false;
  if (take(co_await first.send_to(left, destination)) != left.size())
    co_return false;
  auto partial_batch = faio::spawn(second.recv_many(buffers));
  const auto batch_deadline = std::chrono::steady_clock::now() + 1s;
  while (!partial_batch.done() &&
         std::chrono::steady_clock::now() < batch_deadline)
    co_await faio::time::sleep(1ms);
  if (!partial_batch.done()) {
    partial_batch.request_stop();
    (void)co_await partial_batch;
    co_return false; // 批量余量不得为不存在的下一包挂起。
  }
  auto partial = take(co_await partial_batch);
  if (partial.size() != 1 || partial[0].copied_bytes != left.size() ||
      std::string_view(storage[0].data(), left.size()) != "send")
    co_return false;
  if (take(first.try_send_to(left, destination)) != 4)
    co_return false;
  for (;;) {
    (void)take(co_await second.readable());
    auto peek = second.try_peek_from(empty_buffer);
    if (peek) {
      if (peek->first != 4)
        co_return false;
      break;
    }
    if (peek.error().value() != EAGAIN && peek.error().value() != EWOULDBLOCK)
      co_return false;
  }
  auto owned = take(co_await second.recv_from(faio::io::io_buffer{8}));
  co_return owned.buffer.size() == 4 && owned.message.copied_bytes == 4;
}

/** @brief 输入字符串由 resolver 拥有；localhost 解析与数字快速路径均可 await。
 */
faio::task<bool> dns_owned_inputs() {
  auto context = faio::io::io_context::current();
  constexpr char nul_scope[] = "::1%lo\0tail";
  const auto scope = faio::net::address::numeric_parse(
      std::string_view{nul_scope, sizeof(nul_scope) - 1}, 80);
  if (scope || scope.error().value() != EINVAL)
    co_return false;
  auto numeric = take(co_await faio::net::lookup_host(
      context, std::string{"127.0.0.1"}, std::string{"1234"}));
  if (numeric.size() != 1 || !numeric[0].is_ipv4() || numeric[0].port() != 1234)
    co_return false;
  auto local =
      take(co_await faio::net::lookup_host(std::string{"localhost"}, 2345));
  if (local.empty())
    co_return false;
  for (const auto &endpoint : local)
    if (endpoint.port() != 2345)
      co_return false;
  const auto nul = co_await faio::net::lookup_host(
      context, std::string{"localhost\0suffix", 16}, std::string{"80"});
  co_return !nul && nul.error().value() == EINVAL;
}

/** @brief task 在成员调用时保存资源；后续移动 TCP wrapper 不改变已创建操作。 */
faio::task<bool> tcp_delayed_custom_io() {
  auto listener = take(faio::net::TcpListener::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  auto peer = faio::spawn(echo_vectored(listener));
  auto stream =
      take(co_await faio::net::TcpStream::connect(take(listener.local_addr())));
  std::array<char, 8> payload{'m', 'o', 'v', 'e', 'T', 'C', 'P', '!'}, reply{};
  auto pending =
      stream.async_io(faio::io::Interest::readable,
                      [&](int descriptor) -> faio::expected<std::size_t> {
                        const auto result =
                            ::recv(descriptor, reply.data(), reply.size(), 0);
                        if (result < 0)
                          return std::unexpected{faio::make_error(errno)};
                        return static_cast<std::size_t>(result);
                      });
  auto moved = std::move(stream);
  check(co_await moved.write_all(payload));
  const auto first = take(co_await std::move(pending));
  if (!first)
    co_return false;
  if (first < reply.size())
    check(co_await moved.read_exact(std::span{reply}.subspan(first)));
  co_await peer;
  co_return reply == payload;
}

/** @brief 批量/自定义 UDP 操作在 wrapper 移动后保持原来的 fd/domain。 */
faio::task<bool> udp_delayed_batch_and_custom_io() {
  auto first = take(faio::net::UdpSocket::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  auto second = take(faio::net::UdpSocket::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  const auto destination = take(second.local_addr());
  const std::array<char, 4> payload{'m', 'o', 'v', 'e'};
  const std::array<faio::net::DatagramSend, 1> messages{
      {{payload, destination}}};
  std::array<char, 8> bytes{};
  std::array<std::span<char>, 1> buffers{bytes};
  auto sending = first.send_many(messages);
  auto receiving = second.recv_many(buffers);
  auto source = std::move(first), target = std::move(second);
  if (take(co_await std::move(sending)) != 1)
    co_return false;
  const auto batch = take(co_await std::move(receiving));
  if (batch.size() != 1 || batch[0].copied_bytes != payload.size() ||
      !std::equal(payload.begin(), payload.end(), bytes.begin()))
    co_return false;
  auto custom =
      target.async_io(faio::io::Interest::readable,
                      [&](int descriptor) -> faio::expected<std::size_t> {
                        const auto result =
                            ::recv(descriptor, bytes.data(), bytes.size(), 0);
                        if (result < 0)
                          return std::unexpected{faio::make_error(errno)};
                        return static_cast<std::size_t>(result);
                      });
  auto final_target = std::move(target);
  if (take(co_await source.send_to(payload, destination)) != payload.size())
    co_return false;
  if (take(co_await std::move(custom)) != payload.size())
    co_return false;
  // 最后 wrapper 析构后，预先构造的拥有型操作仍持有控制块，返回明确关闭错误。
  auto expired = [&] {
    auto temporary = take(faio::net::UdpSocket::bind(
        take(faio::net::address::parse("127.0.0.1", 0))));
    return temporary.recv_from(faio::io::io_buffer{8});
  }();
  const auto closed = co_await std::move(expired);
  co_return !closed &&
      (closed.error().value() == EBADF || closed.error().value() == ECANCELED);
}
/** @brief 默认均衡只决定新连接首次归属，listener_local 保留监听者域。 */
faio::task<bool> accepted_connection_placement(std::size_t expected_domains) {
  auto listener = take(faio::net::TcpListener::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  const auto address = take(listener.local_addr());
  std::set<const void *> domains;
  for (std::size_t index = 0; index < expected_domains; ++index) {
    auto client = take(co_await faio::net::TcpStream::connect(address));
    auto accepted = take(co_await listener.accept());
    const auto owner = accepted.first.context().domain();
    if (!owner)
      co_return false;
    domains.insert(owner.get());
    const char payload = static_cast<char>('a' + index);
    check(
        co_await accepted.first.write_all(std::span<const char>{&payload, 1}));
    char received{};
    check(co_await client.read_exact(std::span<char>{&received, 1}));
    if (received != payload || accepted.first.context().domain() != owner)
      co_return false;
    check(co_await accepted.first.close());
    check(co_await client.close());
  }
  if (domains.size() != expected_domains)
    co_return false;
  auto client = take(co_await faio::net::TcpStream::connect(address));
  const faio::net::accept_options local{
      faio::net::accept_placement::listener_local, {}};
  auto accepted = take(co_await listener.accept(local));
  const bool same =
      accepted.first.context().domain() == listener.context().domain();
  check(co_await accepted.first.close());
  check(co_await client.close());
  co_return same;
}

/** @brief 显式目标使用给定引擎；目标停止/空context拒绝，并关闭本次接受的native
 * fd。 */
faio::task<bool> explicit_accept_context(faio::io::io_context target) {
  auto listener = take(faio::net::TcpListener::bind(
      take(faio::net::address::parse("127.0.0.1", 0))));
  const auto address = take(listener.local_addr());
  auto client = take(co_await faio::net::TcpStream::connect(address));
  auto accepted = co_await listener.accept(target);
  if (target.stopped()) {
    if (accepted || accepted.error().value() != ECANCELED)
      co_return false;
    char byte{};
    const auto closed =
        co_await client.read(std::span<char>{&byte, 1}).set_timeout(100ms);
    co_return closed && *closed == 0;
  }
  if (!accepted || accepted->first.context().domain() != target.domain())
    co_return false;
  const char byte = 'e';
  check(co_await accepted->first.write_all(std::span<const char>{&byte, 1}));
  char received{};
  check(co_await client.read_exact(std::span<char>{&received, 1}));
  if (received != byte)
    co_return false;
  check(co_await accepted->first.close());
  check(co_await client.close());
  auto invalid_client = take(co_await faio::net::TcpStream::connect(address));
  const auto invalid = co_await listener.accept(faio::io::io_context{});
  if (invalid || invalid.error().value() != EINVAL)
    co_return false;
  // 空目标可能在接受前就被拒绝；由下一次合法accept消费积压连接，再明确排空。
  auto remaining = co_await listener
                       .accept(faio::net::accept_options{
                           faio::net::accept_placement::listener_local, {}})
                       .set_timeout(20ms);
  if (remaining)
    check(co_await remaining->first.close());
  check(co_await invalid_client.close());
  co_return true;
}
} // namespace

class NetworkContract : public testing::TestWithParam<faio::runtime::mode> {
protected:
  faio::runtime::Config config() const {
    return faio_test::config_builder()
        .set_mode(GetParam())
        .set_num_workers(4)
        .build();
  }
};
TEST_P(NetworkContract, TcpVectoredPeekEmptyReadAndEof) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(tcp_vectored_and_eof()));
}
TEST_P(
    NetworkContract,
    NativeSocketConnectAndEveryBatchAcceptedDescriptorHaveCompletionEvidence) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(tcp_native_connect_and_partial_accept_batch()));
}
TEST_P(NetworkContract, TcpLargeTransferMakesProgressUnderBackpressure) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(tcp_backpressure()));
}
TEST_P(NetworkContract,
       TcpZeroCopyCompletionReturnsBorrowOnlyAfterNativeBufferNotification) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(tcp_zero_copy_releases_borrowed_buffer()));
}
TEST_P(NetworkContract, UdpEmptyDatagramAndFullPayload) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(udp_empty_and_full()));
}
TEST_P(NetworkContract, UdpConnectedSendReceiveAndPeek) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(udp_connected_and_peek()));
}
TEST_P(NetworkContract, TcpOptionsReadyTryOwnedAndNativeInterop) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(tcp_config_ready_native_and_owned()));
}
TEST_P(NetworkContract, UdpMessageTruncationBatchTryPeekAndOwnedBuffer) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(udp_messages_and_batch()));
}
TEST_P(NetworkContract, ResolverOwnsInputsAndRejectsEmbeddedNul) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(dns_owned_inputs()));
}
TEST_P(NetworkContract, TcpCustomIoTaskRemainsValidAfterWrapperMove) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(tcp_delayed_custom_io()));
}
TEST_P(NetworkContract,
       UdpBatchAndCustomIoTasksCaptureResourceBeforeMoveOrDestruction) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(udp_delayed_batch_and_custom_io()));
}
TEST_P(NetworkContract,
       AcceptedConnectionsUseBalancedOrListenerLocalStableDomains) {
  context runtime{config()};
  EXPECT_TRUE(runtime.block_on(accepted_connection_placement(
      GetParam() == faio::runtime::mode::current_thread ? 1 : 4)));
}
TEST_P(NetworkContract,
       ExplicitAcceptContextAndStoppedTargetCloseAcceptedDescriptor) {
  faio::io::io_engine target{faio_test::engine_config()};
  EXPECT_EQ(target.context().balanced_context().domain(),
            target.context().domain());
  context runtime{config()};
  {
    // 独立engine由自己的唯一driver推进；原生Proactor即使立即可写也要采收CQE。
    std::jthread driving([&](std::stop_token stop) {
      while (!stop.stop_requested())
        (void)target.driver().wait_and_drive(10);
    });
    EXPECT_TRUE(runtime.block_on(explicit_accept_context(target.context())));
  } // 先停止唯一driver session，随后shutdown可独占排空控制面。
  target.shutdown();
  EXPECT_TRUE(runtime.block_on(explicit_accept_context(target.context())));
}
INSTANTIATE_TEST_SUITE_P(RuntimeModes, NetworkContract,
                         testing::Values(faio::runtime::mode::current_thread,
                                         faio::runtime::mode::multi_thread));

/** @brief native 注册失败通过 expected 返回；失败 pair 的部分创建必须释放容量。
 */
TEST(NativeRegistrationContract,
     CapacityAndStoppedContextReturnErrorsWithoutThrowing) {
  auto config = faio_test::engine_config();
  config.max_resources = 1;
  faio::io::io_engine engine{std::move(config)};
  const auto context = engine.context();
  {
    auto first = faio::net::TcpSocket::new_v4(context);
    ASSERT_TRUE(first);
    const auto second = faio::net::TcpSocket::new_v4(context);
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().value(), ENFILE);
  }
  // 析构只请求关闭；原生CLOSE完成前资源仍合法占有容量，必须由driver排空。
  for (unsigned attempts = 0; attempts < 100 && !engine.driver().quiescent();
       ++attempts)
    (void)engine.driver().wait_and_drive(10);
  ASSERT_TRUE(engine.driver().quiescent());
  {
    const auto pair = faio::net::unix::pipe::pair(context);
    ASSERT_FALSE(pair);
    EXPECT_EQ(pair.error().value(), ENFILE);
  }
  for (unsigned attempts = 0; attempts < 100 && !engine.driver().quiescent();
       ++attempts)
    (void)engine.driver().wait_and_drive(10);
  ASSERT_TRUE(engine.driver().quiescent());
  auto available = faio::net::TcpSocket::new_v4(context);
  ASSERT_TRUE(available);
  engine.shutdown();
  const auto stopped = faio::net::TcpSocket::new_v4(context);
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().value(), ECANCELED);
}

/** @brief 已注册async fd始终保持非阻塞；导出后用户才可切换原生模式。 */
TEST(NativeRegistrationContract, RegisteredDescriptorCannotDisableNonblocking) {
  faio::io::io_engine engine{faio_test::engine_config()};
  auto socket = take(faio::net::detail::Socket::create(AF_INET, SOCK_STREAM, 0,
                                                       engine.context()));
  ASSERT_TRUE(take(socket.nonblocking()));
  const auto blocking = socket.set_nonblocking(false);
  ASSERT_FALSE(blocking);
  EXPECT_EQ(blocking.error().value(), EINVAL);
  EXPECT_TRUE(take(socket.nonblocking()));
  auto native = take(socket.into_native());
  const int flags = ::fcntl(native.get(), F_GETFL, 0);
  ASSERT_GE(flags, 0);
  ASSERT_EQ(::fcntl(native.get(), F_SETFL, flags & ~O_NONBLOCK), 0);
  EXPECT_EQ(::fcntl(native.get(), F_GETFL, 0) & O_NONBLOCK, 0);
  const auto detached = socket.set_nonblocking(false);
  ASSERT_FALSE(detached);
  EXPECT_EQ(detached.error().value(), EBADF);
}
