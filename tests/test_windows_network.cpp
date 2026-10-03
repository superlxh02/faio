/** @file test_windows_network.cpp
 * @brief Windows 网络集成合同：在 current_thread/multi_thread 上执行同一公开接口。
 * @details 不依赖 POSIX 描述符或 Unix socket；所有等待都有确定的退出条件。
 */
#include "faio/faio.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <source_location>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace std::chrono_literals;

namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

template <class T>
T take(faio::expected<T> result, std::source_location location = std::source_location::current()) {
  if (!result)
    throw std::runtime_error("Windows 网络错误 " + std::to_string(result.error().value()) + " line "
                             + std::to_string(location.line()));
  return std::move(*result);
}

void take(faio::expected<void> result,
          std::source_location location = std::source_location::current()) {
  if (!result)
    throw std::runtime_error("Windows 网络错误 " + std::to_string(result.error().value()) + " line "
                             + std::to_string(location.line()));
}

auto loopback(bool ipv6 = false) -> faio::net::SocketAddr {
  return take(faio::net::SocketAddr::parse(ipv6 ? "::1" : "127.0.0.1", 0));
}

/** @brief 一对连接按值返回，局部监听器析构不会影响两个已接管的 stream。 */
auto connected_pair(bool ipv6 = false)
    -> faio::task<std::pair<faio::net::TcpStream, faio::net::TcpStream>> {
  auto listener = take(faio::net::TcpListener::bind(loopback(ipv6)));
  auto client = take(co_await faio::net::TcpStream::connect(take(listener.local_addr())));
  auto accepted = take(co_await listener.accept().set_timeout(2s));
  require(accepted.second.is_ipv6() == ipv6, "accept 地址族错误");
  take(co_await listener.close());
  co_return std::pair{std::move(client), std::move(accepted.first)};
}

/** @brief 数字解析、原生宽度和控制选项的同步合同。 */
void address_and_options() {
  static_assert(sizeof(decltype(std::declval<faio::net::TcpStream>().fd())) == sizeof(SOCKET));
  static_assert(!std::is_copy_constructible_v<faio::net::owned_native_socket>);
  auto v4 = take(faio::net::SocketAddr::parse("127.0.0.1:2345"));
  auto v6 = take(faio::net::SocketAddr::parse("[::1%4]:3456"));
  require(v4.to_string() == "127.0.0.1:2345", "IPv4 格式错误");
  require(v6.scope_id() == 4 && v6.port() == 3456, "IPv6 scope 未保留");
  require(!faio::net::SocketAddr::parse("localhost:80"), "数字解析进行了 DNS");
  require(!faio::net::SocketAddr::parse("127.0.0.1:65536"), "端口被截断");
  require(!faio::net::SocketAddr::numeric_parse(std::string_view{"::1%4\0tail", 10}, 80),
          "NUL 未拒绝");
  auto socket = take(faio::net::TcpSocket::new_v4());
  take(socket.set_nodelay(true));
  take(socket.set_keepalive(true));
  take(socket.set_recv_buffer_size(4096));
  take(socket.set_send_buffer_size(4096));
  take(socket.set_linger(1s));
  require(take(socket.nodelay()) && take(socket.keepalive()), "TCP 选项错误");
  require(take(socket.recv_buffer_size()) >= 4096 && take(socket.send_buffer_size()) >= 4096,
          "缓冲区选项错误");
  require(take(socket.linger()) == 1s, "linger 大小或字段错误");
  require(!socket.set_linger(65536s), "Windows linger 被截断");
  take(socket.set_linger({}));
  take(socket.set_reuseaddr(true));
  require(take(socket.reuseaddr()), "reuseaddr 错误");
  take(socket.bind(loopback()));
  take(socket.set_ttl(64));
  require(take(socket.ttl()) == 64, "TTL 错误");
  require(!take(socket.take_error()), "新 socket 有挂起错误");
  auto native = take(socket.into_native());
  const auto descriptor = native.get();
  auto imported = take(faio::net::TcpSocket::from_native({}, std::move(native)));
  require(imported.fd() == descriptor, "native socket 被截断或复制");
  auto listener = take(std::move(imported).listen());
  auto owned_listener = take(listener.into_native());
  auto restored = take(faio::net::TcpListener::from_native({}, std::move(owned_listener)));
  require(take(restored.local_addr()).port() != 0, "listener native 导入错误");
  auto unbound = take(faio::net::UdpSocket::unbound());
  take(unbound.set_broadcast(true));
  require(take(unbound.broadcast()), "broadcast 错误");
  take(unbound.set_multicast_loop_v4(true));
  take(unbound.set_multicast_ttl_v4(3));
  require(take(unbound.multicast_loop_v4()) && take(unbound.multicast_ttl_v4()) == 3,
          "Windows DWORD 组播选项错误");
  auto udp_native = take(unbound.into_native());
  auto udp = take(faio::net::UdpSocket::from_native({}, std::move(udp_native)));
  take(udp.set_recv_packet_info_v4(true));
}

/** @brief TCP 单次、聚集、拥有型缓冲、就绪、半关闭、半边重合及超时。 */
auto tcp_contract() -> faio::task<void> {
  auto [client, server] = co_await connected_pair();
  // 已关联 IOCP 的 socket 只在相同 owner 中重新导入，系统完成端口关联保持不变。
  const auto owner = client.context();
  auto exported = take(client.into_native());
  client = take(faio::net::TcpStream::from_native(owner, std::move(exported)));
  require(take(client.peer_addr()) == take(server.local_addr()),
          "连接 stream 原生导入丢失 socket 上下文");
  take(client.set_nodelay(true));
  require(take(co_await client.writable().set_timeout(2s)).is_writable(), "writable 未完成");
  std::array<char, 8> buffer{};
  auto empty_try = client.try_read(buffer);
  require(!empty_try && faio::net::detail::socket_would_block(empty_try.error()),
          "try_read 空队列不是 would-block");
  // TCP 零容量读取没有需要等待的 payload；低层入口也应立即成功。
  require(
      take(co_await faio::io::recv(client.resource(), buffer.data(), 0).set_timeout(100ms)) == 0,
      "raw TCP 零容量读发生等待");
  require(take(co_await faio::io::recv(client.resource(), buffer.data(), 0, MSG_DONTWAIT)
                   .set_timeout(100ms))
              == 0,
          "raw TCP 零容量非阻塞读失败");
  const auto peek_expired =
      co_await faio::io::recv(client.resource(), buffer.data(), buffer.size(), MSG_PEEK)
          .set_timeout(10ms);
  require(!peek_expired && peek_expired.error().value() == ETIMEDOUT,
          "空 TCP PEEK 的轮询取消没有交付 timeout");
  require(take(co_await client.read(std::span<char>{})) == 0, "空读发生 IO");
  require(take(co_await client.write(std::span<const char>{})) == 0, "空写发生 IO");
  std::array<char, 4> first{'f', 'a', 'i', 'o'}, second{'I', 'O', 'C', 'P'};
  require(take(co_await client.write_vectored(first, second)) == 8, "聚集发送字节数错误");
  require(take(co_await server.readable().set_timeout(2s)).is_readable(), "readable 未完成");
  require(take(co_await server.peek(buffer).set_timeout(2s)) > 0, "TCP peek 未读取数据");
  std::array<iovec, 2> vectors{{{buffer.data(), 4}, {buffer.data() + 4, 4}}};
  auto read = take(co_await server.read_vectored(std::span<iovec>{vectors}).set_timeout(2s));
  if (read < buffer.size())
    take(co_await server.read_exact(std::span<char>{buffer}.subspan(read)));
  require(std::equal(first.begin(), first.end(), buffer.begin())
              && std::equal(second.begin(), second.end(), buffer.begin() + 4),
          "聚集内容错误");
  take(co_await server.write_all(buffer));
  auto owned = take(co_await client.read(faio::io::io_buffer{8}));
  require(owned.bytes > 0 && owned.buffer.size() == owned.bytes, "拥有型读未发布初始化范围");
  if (owned.bytes < 8)
    take(co_await client.read_exact(std::span<char>{buffer}.first(8 - owned.bytes)));
  auto shared = faio::io::shared_const_buffer{std::string_view{"shared"}};
  require(take(co_await client.write(shared)) == 6, "shared buffer 发送错误");
  std::array<char, 6> storage{};
  faio::io::read_buf initialized{storage};
  require(take(co_await server.read_buf(initialized)) > 0, "read_buf 未推进");
  if (initialized.size() < storage.size())
    take(co_await server.read_exact(initialized.unfilled()));
  require(std::string_view{storage.data(), storage.size()} == "shared", "read_buf 内容错误");
  require(take(co_await client.write_zc(std::span<const char>{first})) == 4,
          "Windows zero-copy fallback 错误");
  std::array<char, 4> received{};
  take(co_await server.read_bytes(received));
  require(received == first, "zero-copy fallback 内容错误");
  auto [reader, writer] = std::move(client).into_split();
  require(reader.resource() == writer.resource(), "split 复制了 native socket");
  require(take(co_await writer.write_owned(std::vector<char>{'s', 'p', 'l', 't'})) == 4,
          "owned half 写入错误");
  take(co_await server.read_exact(received));
  auto reunited = take(std::move(reader).reunite(std::move(writer)));
  auto expired = co_await reunited.read(received).set_timeout(10ms);
  require(!expired && expired.error().value() == ETIMEDOUT, "IO deadline 未产生 ETIMEDOUT");
  take(co_await reunited.write_all(std::span<const char>{first}));
  take(co_await server.read_exact(received));
  auto observing = server.async_io(
      faio::io::Interest::readable,
      [&received](faio::io::detail::native_descriptor fd) -> faio::expected<std::size_t> {
        const auto result = ::recv(static_cast<SOCKET>(fd), received.data(), 4, 0);
        if (result < 0)
          return std::unexpected{faio::net::detail::socket_error()};
        return static_cast<std::size_t>(result);
      });
  auto moved = std::move(server);
  take(co_await reunited.write_all(std::span<const char>{first}));
  require(take(co_await std::move(observing)) > 0, "移动后 async_io 资源丢失");
  take(co_await reunited.shutdown());
  require(take(co_await moved.read(received).set_timeout(2s)) == 0, "写半关闭未交付 EOF");
  take(co_await moved.write_all(std::span<const char>{first}));
  take(co_await reunited.read_exact(received));
  require(received == first, "写半关闭破坏读方向");
  take(co_await reunited.flush());
  take(co_await moved.close());
  take(co_await reunited.close());
  // IPv6 覆盖 ConnectEx 自动绑定和 AcceptEx 地址解码。
  auto pair6 = co_await connected_pair(true);
  take(co_await pair6.first.write_all(std::span<const char>{first}));
  take(co_await pair6.second.read_exact(received));
  require(received == first, "IPv6 TCP 内容错误");
  co_return;
}

/** @brief Windows 数据报消息边界、零包、截断、辅助数据和批次。 */
auto udp_contract() -> faio::task<void> {
  auto first = take(faio::net::UdpSocket::bind(loopback()));
  auto second = take(faio::net::UdpSocket::bind(loopback()));
  auto target = take(second.local_addr());
  auto source = take(first.local_addr());
  take(second.set_recv_packet_info_v4(true));
  std::array<char, 16> message{'u', 'd', 'p', '!', 'b', 'o', 'u', 'n', 'd', 'a', 'r', 'y'};
  require(take(co_await first.send_to(std::span<const char>{}, target)) == 0,
          "零长度 UDP 发送失败");
  std::array<char, 32> buffer{};
  auto zero = take(co_await second.recv_message(buffer).set_timeout(2s));
  require(zero.copied_bytes == 0 && zero.peer == source && !zero.truncated,
          "零长度 UDP 被解释为 EOF");
  require(take(co_await first.send_to(message, target)) == message.size(), "UDP 发送长度错误");
  std::array<std::byte, 128> control{};
  auto packet =
      take(co_await second.recv_message(std::span<char>{buffer}.first(4), control).set_timeout(2s));
  require(packet.copied_bytes == 4 && packet.truncated && packet.peer == source,
          "WSAEMSGSIZE 没有转换为 payload 截断结果");
  require(packet.control_bytes >= CMSG_SPACE(sizeof(IN_PKTINFO)),
          "WSARecvMsg 没有返回 packet info");
  msghdr controls{};
  controls.msg_control = control.data();
  controls.msg_controllen = packet.control_bytes;
  const auto* header = CMSG_FIRSTHDR(&controls);
  require(header && header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO,
          "控制消息格式错误");
  take(co_await first.connect(target));
  take(co_await second.connect(source));
  require(take(co_await first.send(message)) == message.size(), "连接 UDP send 错误");
  auto peek = take(co_await second.peek_from(buffer).set_timeout(2s));
  require(peek.first == message.size() && peek.second == source,
          "UDP peek 消费了消息或返回错误地址");
  auto consumed = take(co_await second.recv_from(faio::io::io_buffer{32}));
  require(
      consumed.message.copied_bytes == message.size() && consumed.buffer.size() == message.size(),
      "owned UDP 接收错误");
  std::array<faio::net::DatagramSend, 3> outgoing{
      {{std::span<const char>{message}.first(4), target},
       {std::span<const char>{message}.first(8), {}},
       {std::span<const char>{}, target}}};
  require(take(co_await first.send_many(outgoing)) == outgoing.size(),
          "UDP send_many 未保留包数量");
  std::array<std::array<char, 32>, 3> incoming{};
  std::array<std::span<char>, 3> ranges{incoming[0], incoming[1], incoming[2]};
  auto batch = take(co_await second.recv_many(ranges));
  require(batch.size() == 3 && batch[0].copied_bytes == 4 && batch[1].copied_bytes == 8
              && batch[2].copied_bytes == 0,
          "UDP recv_many 合并或等待了不存在的数据报");
  auto shared = first.share();
  require(shared.resource() == first.resource(), "UDP share 复制了 socket");
  std::array<iovec, 2> vectors{{{message.data(), 4}, {message.data() + 4, 4}}};
  require(take(co_await shared.send_message(vectors, target)) == 8, "UDP 向量消息发送错误");
  require(take(co_await second.recv(buffer).set_timeout(2s)) == 8, "UDP 向量消息被拆包");
  auto no_packet = second.try_recv(buffer);
  require(!no_packet && faio::net::detail::socket_would_block(no_packet.error()),
          "UDP try_recv 空队列不是 would-block");
  require(take(shared.try_send_to(std::span<const char>{message}.first(4), target)) == 4,
          "UDP try_send_to 失败");
  require(take(co_await second.readable().set_timeout(2s)).is_readable(), "UDP readiness 未完成");
  require(take(second.try_peek_from(buffer)).first == 4
              && take(second.try_recv_from(buffer)).first == 4,
          "UDP try_peek/try_recv 语义错误");
  take(co_await first.close());
  take(co_await second.close());
  co_return;
}

/** @brief 低层 recv/recvfrom/recvmsg 统一交付 UDP 已复制长度，PEEK 保留消息边界。
 * @details 同时覆盖 IOCP 挂起路径和 MSG_DONTWAIT 短系统调用路径，防止后者误挂起。
 */
auto raw_udp_contract() -> faio::task<void> {
  auto sender = take(faio::net::UdpSocket::bind(loopback()));
  auto receiver = take(faio::net::UdpSocket::bind(loopback()));
  const auto target = take(receiver.local_addr());
  const auto source = take(sender.local_addr());
  std::array<char, 16> payload{
      'r', 'a', 'w', '-', 'd', 'a', 't', 'a', 'g', 'r', 'a', 'm', '1', '2', '3', '4'};
  std::array<char, 32> buffer{};
  for (const int nonblocking : {0, MSG_DONTWAIT}) {
    // sendto/recvfrom 的 raw descriptor 重载也必须保留完整 SOCKET 位宽。
    require(take(co_await faio::io::sendto(sender.fd(),
                                           payload.data(),
                                           payload.size(),
                                           MSG_DONTWAIT,
                                           target.sockaddr(),
                                           target.length())
                     .set_timeout(2s))
                == payload.size(),
            "raw sendto MSG_DONTWAIT 失败");
    take(co_await receiver.readable().set_timeout(2s));
    faio::net::SocketAddr peer;
    socklen_t peer_length = peer.capacity();
    require(take(co_await faio::io::recvfrom(
                     receiver.fd(), buffer.data(), 4, nonblocking, peer.sockaddr(), &peer_length)
                     .set_timeout(2s))
                    == 4
                && peer == source
                && std::equal(payload.begin(), payload.begin() + 4, buffer.begin()),
            "raw recvfrom 截断没有交付已复制字节或 peer");
    peer_length = peer.capacity();
    const auto empty = co_await faio::io::recvfrom(receiver.resource(),
                                                   buffer.data(),
                                                   buffer.size(),
                                                   MSG_DONTWAIT,
                                                   peer.sockaddr(),
                                                   &peer_length)
                           .set_timeout(100ms);
    require(!empty && faio::net::detail::socket_would_block(empty.error()),
            "raw recvfrom MSG_DONTWAIT 误挂起或截断残余成为第二个包");
    take(co_await sender.send_to(payload, target));
    take(co_await receiver.readable().set_timeout(2s));
    peer_length = peer.capacity();
    require(take(co_await faio::io::recvfrom(receiver.resource(),
                                             buffer.data(),
                                             4,
                                             nonblocking | MSG_PEEK,
                                             peer.sockaddr(),
                                             &peer_length)
                     .set_timeout(2s))
                    == 4
                && peer == source,
            "raw recvfrom PEEK 小缓冲失败");
    peer_length = peer.capacity();
    require(
        take(
            co_await faio::io::recvfrom(
                receiver.resource(), buffer.data(), buffer.size(), 0, peer.sockaddr(), &peer_length)
                .set_timeout(2s))
                == payload.size()
            && std::equal(payload.begin(), payload.end(), buffer.begin()),
        "raw recvfrom PEEK 消费了数据报或截断了原消息");
  }
  take(co_await sender.connect(target));
  take(co_await receiver.connect(source));
  for (const int nonblocking : {0, MSG_DONTWAIT}) {
    require(take(co_await faio::io::send(
                     sender.resource(), payload.data(), payload.size(), MSG_DONTWAIT)
                     .set_timeout(2s))
                == payload.size(),
            "raw send MSG_DONTWAIT 失败");
    take(co_await receiver.readable().set_timeout(2s));
    require(take(co_await faio::io::recv(receiver.resource(), buffer.data(), 4, nonblocking)
                     .set_timeout(2s))
                    == 4
                && std::equal(payload.begin(), payload.begin() + 4, buffer.begin()),
            "connected raw recv 截断返回了 WSAEMSGSIZE");
    const auto empty =
        co_await faio::io::recv(receiver.resource(), buffer.data(), buffer.size(), MSG_DONTWAIT)
            .set_timeout(100ms);
    require(!empty && faio::net::detail::socket_would_block(empty.error()),
            "raw recv MSG_DONTWAIT 误挂起或出现残余数据报");
    take(co_await sender.send(payload));
    take(co_await receiver.readable().set_timeout(2s));
    require(
        take(co_await faio::io::recv(receiver.resource(), buffer.data(), 4, nonblocking | MSG_PEEK)
                 .set_timeout(2s))
            == 4,
        "connected raw recv PEEK 小缓冲失败");
    require(take(co_await faio::io::recv(receiver.resource(), buffer.data(), buffer.size())
                     .set_timeout(2s))
                    == payload.size()
                && std::equal(payload.begin(), payload.end(), buffer.begin()),
            "connected raw recv PEEK 消费了数据报");

    // 无 name/control 的 recvmsg 使用 WSARecv；必须和 WSARecvMsg 保持相同截断合同。
    for (const int peeking : {0, MSG_PEEK}) {
      take(co_await sender.send(payload));
      take(co_await receiver.readable().set_timeout(2s));
      iovec vector{buffer.data(), 4};
      msghdr message{};
      message.msg_iov = &vector;
      message.msg_iovlen = 1;
      require(take(co_await faio::io::recvmsg(receiver.resource(), &message, nonblocking | peeking)
                       .set_timeout(2s))
                      == 4
                  && (message.msg_flags & MSG_TRUNC) && message.msg_controllen == 0
                  && std::equal(payload.begin(), payload.begin() + 4, buffer.begin()),
              "connected raw recvmsg 丢失 copied bytes 或 MSG_TRUNC");
      if (peeking)
        require(take(co_await receiver.recv(buffer).set_timeout(2s)) == payload.size(),
                "connected raw recvmsg PEEK 消费了数据报");
      message.msg_flags = 0;
      const auto no_message =
          co_await faio::io::recvmsg(receiver.resource(), &message, MSG_DONTWAIT)
              .set_timeout(100ms);
      require(!no_message && faio::net::detail::socket_would_block(no_message.error()),
              "raw recvmsg MSG_DONTWAIT 误挂起或出现残余数据报");
    }
  }
  for (const int nonblocking : {0, MSG_DONTWAIT}) {
    require(
        take(co_await faio::io::send(sender.resource(), nullptr, 0, nonblocking).set_timeout(2s))
            == 0,
        "raw send 零长度 UDP 失败");
    require(take(co_await faio::io::recv(receiver.resource(), buffer.data(), buffer.size())
                     .set_timeout(2s))
                == 0,
            "raw recv 没有返回合法零长度 UDP 包");
    require(take(co_await faio::io::sendto(
                     sender.resource(), nullptr, 0, nonblocking, target.sockaddr(), target.length())
                     .set_timeout(2s))
                == 0,
            "raw sendto 零长度 UDP 失败");
    faio::net::SocketAddr peer;
    socklen_t peer_length = peer.capacity();
    require(
        take(
            co_await faio::io::recvfrom(
                receiver.resource(), buffer.data(), buffer.size(), 0, peer.sockaddr(), &peer_length)
                .set_timeout(2s))
                == 0
            && peer == source,
        "raw recvfrom 零长度 UDP 丢失 peer");
  }
  // 零容量缓冲同样消费一个包；零字节结果不可误判为 TCP EOF 或原生错误。
  take(co_await sender.send(payload));
  take(co_await receiver.readable().set_timeout(2s));
  require(take(co_await faio::io::recv(receiver.resource(), buffer.data(), 0).set_timeout(2s)) == 0,
          "raw recv 零容量 UDP 失败");
  const auto discarded =
      co_await faio::io::recv(receiver.resource(), buffer.data(), buffer.size(), MSG_DONTWAIT)
          .set_timeout(100ms);
  require(!discarded && faio::net::detail::socket_would_block(discarded.error()),
          "raw recv 零容量没有消费 UDP 包");
  take(co_await sender.close());
  take(co_await receiver.close());
}

/** @brief accept 批次、预配置 socket 连接和 resolver 字符串拥有性。 */
auto listener_and_resolver() -> faio::task<void> {
  const auto context = faio::io::io_context::current();
  auto numeric = take(co_await faio::net::lookup_host(context, "127.0.0.1", std::string{"2345"}));
  require(numeric.size() == 1 && numeric.front().port() == 2345, "数字 resolver 快速路径错误");
  auto names = take(co_await faio::net::lookup_host(std::string{"localhost"}, 3456));
  require(!names.empty() && names.front().port() == 3456, "DNS lane 解析失败");
  auto invalid =
      co_await faio::net::lookup_host(context, std::string{"local\0host", 10}, std::string{"80"});
  require(!invalid && invalid.error().value() == EINVAL, "DNS NUL 未拒绝");
  auto socket = take(faio::net::TcpSocket::new_v4(context));
  take(socket.set_nodelay(true));
  take(socket.bind(loopback()));
  auto listener = take(std::move(socket).listen(16));
  const auto destination = take(listener.local_addr());
  auto none = listener.try_accept();
  require(!none && faio::net::detail::socket_would_block(none.error()), "try_accept 空队列阻塞");
  std::vector<faio::net::TcpStream> clients;
  for (int index = 0; index < 3; ++index) {
    auto client_socket = take(faio::net::TcpSocket::new_v4());
    take(client_socket.bind(loopback()));
    clients.push_back(take(co_await std::move(client_socket).connect(destination)));
  }
  auto accepted = take(co_await listener.accept_many(8));
  require(!accepted.empty() && accepted.size() <= 3, "accept_many 批次错误");
  while (accepted.size() < clients.size())
    accepted.push_back(take(listener.try_accept()));
  for (auto& pair : accepted)
    take(co_await pair.first.close());
  for (auto& client : clients)
    take(co_await client.close());
  take(co_await listener.close());
  co_return;
}

/** @brief 大包在发送背压下正常推进，缓冲区在读取完成前保持拥有。 */
auto receive_large(faio::net::TcpStream stream, std::size_t total) -> faio::task<void> {
  std::vector<char> buffer(total);
  take(co_await stream.read_exact(buffer));
  require(std::all_of(buffer.begin(), buffer.end(), [](char value) { return value == 'x'; }),
          "背压数据损坏");
  take(co_await stream.close());
}

auto backpressure() -> faio::task<void> {
  auto pair = co_await connected_pair();
  take(pair.first.set_send_buffer_size(4096));
  constexpr std::size_t size = 1024 * 1024;
  auto receiver = faio::spawn(receive_large(std::move(pair.second), size));
  std::vector<char> payload(size, 'x');
  take(co_await pair.first.write_all(payload));
  co_await receiver;
  take(co_await pair.first.close());
}

/** @brief 两个观察者共享同一 ready，不取得读方向 gate，也不会消费实际字节。 */
auto observe(faio::io::detail::resource_ptr resource)
    -> faio::task<faio::expected<faio::io::Ready>> {
  co_return co_await faio::io::ready(std::move(resource), faio::io::Interest::readable)
      .set_timeout(2s);
}

/** @brief 取消排空以前帧内借用缓冲始终存活，结果通过原 awaiter 唯一交付。 */
auto wait_read(faio::io::detail::resource_ptr resource, std::span<char> buffer, int flags = 0)
    -> faio::task<faio::expected<std::size_t>> {
  co_return co_await faio::io::recv(std::move(resource), buffer.data(), buffer.size(), flags)
      .set_timeout(2s);
}

auto observers_and_close() -> faio::task<void> {
  auto [client, server] = co_await connected_pair();
  auto first = faio::spawn(observe(server.resource()));
  auto second = faio::spawn(observe(server.resource()));
  // 确保至少经历一次真实挂起，再由网络事件完成两个独立观察者。
  co_await faio::time::sleep(5ms);
  const char byte = 'o';
  take(co_await client.write_all(std::span<const char>{&byte, 1}));
  require(take(co_await first).is_readable() && take(co_await second).is_readable(),
          "多个 ready 观察者没有同时完成");
  char received{};
  take(co_await server.read_exact(std::span<char>{&received, 1}));
  require(received == byte, "ready 消费了网络数据");
  auto reading = faio::spawn(wait_read(server.resource(), std::span<char>{&received, 1}));
  co_await faio::time::sleep(5ms);
  take(co_await server.close());
  const auto cancelled = co_await reading;
  require(
      !cancelled && (cancelled.error().value() == ECANCELED || cancelled.error().value() == EBADF),
      "close 未取消并排空挂起的原生读取");
  take(co_await client.close());
  // PEEK 必须在数据到达以前也能挂起；完成后原消息仍由正常 read 领取。
  auto pair = co_await connected_pair();
  auto peeking =
      faio::spawn(wait_read(pair.second.resource(), std::span<char>{&received, 1}, MSG_PEEK));
  co_await faio::time::sleep(5ms);
  take(co_await pair.first.write_all(std::span<const char>{&byte, 1}));
  require(take(co_await peeking) == 1 && received == byte, "等待中的 TCP PEEK 没有随就绪完成");
  take(co_await pair.second.read_exact(std::span<char>{&received, 1}));
  auto cancelled_peek =
      faio::spawn(wait_read(pair.second.resource(), std::span<char>{&received, 1}, MSG_PEEK));
  co_await faio::time::sleep(5ms);
  take(co_await pair.second.close());
  const auto closed_peek = co_await cancelled_peek;
  require(!closed_peek
              && (closed_peek.error().value() == ECANCELED || closed_peek.error().value() == EBADF),
          "close 未取消和摘除挂起的 PEEK 观察者");
  take(co_await pair.first.close());
}

/** @brief 原生拥有者保存不可迁移的 IOCP 归属，并在 raw release/析构时清缓存。 */
auto native_export_contract() -> faio::task<void> {
  {
    auto [client, server] = co_await connected_pair();
    faio::io::io_engine different;
    auto exported = take(client.into_native());
    auto wrong_domain = faio::net::TcpStream::from_native(different.context(), std::move(exported));
    require(!wrong_domain && wrong_domain.error().value() == EXDEV,
            "已关联 IOCP handle 被迁移到不同域");
    char byte{};
    require(take(co_await server.read(std::span<char>{&byte, 1}).set_timeout(2s)) == 0,
            "失败 native import 没有关闭唯一原生拥有者");
  }
  for (unsigned index = 0; index < 8; ++index) {
    auto [client, server] = co_await connected_pair();
    auto native = take(client.into_native());
    const auto descriptor = native.release();
    require(::closesocket(static_cast<SOCKET>(descriptor)) == 0, "raw release 没有转交关闭责任");
    char byte{};
    require(take(co_await server.read(std::span<char>{&byte, 1}).set_timeout(2s)) == 0,
            "raw native close 没有产生 EOF");
    // 下次 socket 可复用相同数值；旧 IOCP 注册缓存不能跳过新 handle 的关联。
  }
  {
    faio::io::io_engine engine;
    auto context = engine.context();
    auto socket = take(faio::net::TcpSocket::new_v4(context));
    auto native = take(socket.into_native());
    engine.shutdown();
    auto stopped = faio::net::TcpSocket::from_native(context, std::move(native));
    require(!stopped && stopped.error().value() == ECANCELED, "停止域接受了已关联的原生 handle");
  }
  co_return;
}
}  // namespace

int main() {
  try {
    address_and_options();
    for (const auto mode :
         {faio::runtime::mode::current_thread, faio::runtime::mode::multi_thread}) {
      auto config = faio::config_builder{}.set_mode(mode).set_num_workers(2).build();
      faio::runtime::detail::runtime_context runtime{config};
      std::cout << "TCP mode " << static_cast<int>(mode) << std::endl;
      runtime.block_on(tcp_contract());
      std::cout << "UDP mode " << static_cast<int>(mode) << std::endl;
      runtime.block_on(udp_contract());
      std::cout << "Raw UDP mode " << static_cast<int>(mode) << std::endl;
      runtime.block_on(raw_udp_contract());
      std::cout << "Resolver/listener mode " << static_cast<int>(mode) << std::endl;
      runtime.block_on(listener_and_resolver());
      std::cout << "Backpressure mode " << static_cast<int>(mode) << std::endl;
      runtime.block_on(backpressure());
      std::cout << "Observers/close mode " << static_cast<int>(mode) << std::endl;
      runtime.block_on(observers_and_close());
      std::cout << "Native export mode " << static_cast<int>(mode) << std::endl;
      runtime.block_on(native_export_contract());
    }
    std::cout << "Windows network contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
