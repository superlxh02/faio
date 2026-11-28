/**
 * @file udp_echo.cpp
 * @brief 一个持续监听的 UDP 回显服务端。
 * @details 服务绑定 127.0.0.1:8081，每收到一个数据报就把它原样发回来源地址。
 * 按 Ctrl+C 结束进程。
 */
#include <faio/faio.hpp>

#include <array>
#include <exception>
#include <span>

namespace {
const auto address = faio::net::socket_addr{faio::net::ipv4_addr{127, 0, 0, 1}, 8081};

/** @brief 绑定端口并持续回显数据报。 */
faio::task<int> serve() {
  auto socket = faio::net::udp_socket::bind(address);
  if (!socket) {
    faio::log::logger()->error("绑定 {} 失败：{}", address.to_string(), socket.error().message());
    co_return 1;
  }

  faio::log::logger()->info("UDP 回显服务监听 {}", address.to_string());
  std::array<char, 65507> buffer{};
  for (;;) {
    auto datagram = co_await socket->recv_from(buffer);
    if (!datagram) {
      faio::log::logger()->error("接收数据报失败：{}", datagram.error().message());
      co_return 1;
    }

    auto [size, peer] = *datagram;
    const auto sent = co_await socket->send_to(std::span<const char>{buffer}.first(size), peer);
    if (!sent) {
      faio::log::logger()->error("发回 {} 失败：{}", peer.to_string(), sent.error().message());
      co_return 1;
    }
    if (*sent != size) {
      faio::log::logger()->error("发回 {} 时只写入 {} / {} 字节", peer.to_string(), *sent, size);
      co_return 1;
    }
  }
}
}  // namespace

/** @brief 运行 UDP 回显服务，退出时返回非零表示启动或收发失败。 */
int main() {
  try {
    return faio::block_on(serve());
  } catch (const std::exception& error) {
    faio::log::logger()->error("UDP 回显服务异常退出：{}", error.what());
    return 1;
  }
}
