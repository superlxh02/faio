#include "faio/faio.hpp"
#include "fastlog/fastlog.hpp"

// ============================================================================
// 示例: UDP echo server
// 绑定端口后循环 recv_from / send_to，将收到的内容回显给对端。
// ============================================================================

faio::task<void> server(uint16_t port) {
  auto addr = faio::net::address::parse("0.0.0.0", port);
  if (!addr) {
    fastlog::console.error("  parse address failed");
    co_return;
  }

  auto has_socket = faio::net::UdpDatagram::bind(addr.value());
  if (!has_socket) {
    fastlog::console.error("  bind failed: {}", has_socket.error().message());
    co_return;
  }

  fastlog::console.info("  udp echo server listening on 0.0.0.0:{}", port);
  auto socket = std::move(has_socket.value());
  char buf[1024];

  while (true) {
    auto result = co_await socket.recv_from(buf);
    if (!result) {
      fastlog::console.error("  recv_from failed: {}",
                             result.error().message());
      break;
    }

    auto &[len, peer_addr] = result.value();
    buf[len] = '\0';
    fastlog::console.info("  received {} bytes from {}: {}", len, peer_addr,
                          buf);

    auto send_result = co_await socket.send_to({buf, len}, peer_addr);
    if (!send_result) {
      fastlog::console.error("  send_to failed: {}",
                             send_result.error().message());
      break;
    }
  }
}

int main() {
  fastlog::set_consolelog_level(fastlog::LogLevel::Info);
  fastlog::console.info("===== 示例: UDP echo server =====");
  faio::runtime_context ctx;
  faio::block_on<void>(ctx, server(9090));
  fastlog::console.info("===== udp server done =====");
  return 0;
}
