#include "faio/faio.hpp"
#include "faio/log.hpp"
#include <string_view>

// ============================================================================
// 示例: UDP echo server
// 绑定端口后循环 recv_from / send_to，将收到的内容回显给对端。
// ============================================================================

faio::task<void> server(uint16_t port) {
  auto addr = faio::net::address::parse("0.0.0.0", port);
  if (!addr) {
    faio::log::logger()->error("  parse address failed");
    co_return;
  }

  auto has_socket = faio::net::UdpDatagram::bind(addr.value());
  if (!has_socket) {
    faio::log::logger()->error("  bind failed: {}", has_socket.error().message());
    co_return;
  }

  faio::log::logger()->info("  udp echo server listening on 0.0.0.0:{}", port);
  auto socket = std::move(has_socket.value());
  char buf[1024];

  while (true) {
    auto result = co_await socket.recv_from(buf);
    if (!result) {
      faio::log::logger()->error("  recv_from failed: {}",
                             result.error().message());
      break;
    }

    auto &[len, peer_addr] = result.value();
    faio::log::logger()->trace("udp received {} bytes from {}: {}", len,
                              peer_addr.to_string(), std::string_view{buf, len});

    auto send_result = co_await socket.send_to({buf, len}, peer_addr);
    if (!send_result) {
      faio::log::logger()->error("  send_to failed: {}",
                             send_result.error().message());
      break;
    }
  }
}

int main() {
  faio::log::logger()->set_level(spdlog::level::info);
  faio::log::logger()->info("===== 示例: UDP echo server =====");
  faio::block_on<void>(server(9090));
  faio::log::logger()->info("===== udp server done =====");
  return 0;
}
