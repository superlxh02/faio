/**
 * @file tcp_echo.cpp
 * @brief 一个持续监听的 TCP 回显服务端。
 * @details 服务绑定 127.0.0.1:8080，接受连接后交给独立任务处理；每条连接持续
 * 读取字节并原样写回，直到对端关闭或发生错误。按 Ctrl+C 结束进程。
 */
#include <faio/faio.hpp>

#include <array>
#include <exception>
#include <span>
#include <utility>

namespace {
const auto address = faio::net::SocketAddr{faio::net::Ipv4Addr{127, 0, 0, 1}, 8080};

/** @brief 持续读取一条连接并回写，直到对端关闭或 I/O 失败。 */
faio::task<void> handle_connection(faio::net::TcpStream stream,
                                   faio::net::SocketAddr peer) {
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto count = co_await stream.read(buffer);
    if (!count) {
      faio::log::logger()->error("读取 {} 失败：{}", peer.to_string(), count.error().message());
      co_return;
    }
    if (*count == 0) {
      faio::log::logger()->info("{} 已关闭连接", peer.to_string());
      co_return;
    }

    const auto written =
        co_await stream.write_all(std::span<const char>{buffer}.first(*count));
    if (!written) {
      faio::log::logger()->error("写回 {} 失败：{}", peer.to_string(), written.error().message());
      co_return;
    }
  }
}

/** @brief 绑定端口并持续接受连接。 */
faio::task<int> serve() {
  auto listener = faio::net::TcpListener::bind(address);
  if (!listener) {
    faio::log::logger()->error("绑定 {} 失败：{}", address.to_string(), listener.error().message());
    co_return 1;
  }

  faio::log::logger()->info("TCP 回显服务监听 {}", address.to_string());
  for (;;) {
    auto accepted = co_await listener->accept();
    if (!accepted) {
      faio::log::logger()->error("接受连接失败：{}", accepted.error().message());
      co_return 1;
    }

    auto [stream, peer] = std::move(*accepted);
    faio::log::logger()->info("接受来自 {} 的连接", peer.to_string());
    faio::spawn_detached(handle_connection(std::move(stream), peer));
  }
}
}  // namespace

/** @brief 运行 TCP 回显服务，退出时返回非零表示启动或接受连接失败。 */
int main() {
  try {
    return faio::block_on(serve());
  } catch (const std::exception& error) {
    faio::log::logger()->error("TCP 回显服务异常退出：{}", error.what());
    return 1;
  }
}
