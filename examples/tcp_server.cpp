#include "faio/faio.hpp"
#include "fastlog/fastlog.hpp"

// ============================================================================
// 示例: TCP echo server
// ============================================================================

auto echo(faio::net::TcpStream stream) -> faio::task<void> {
  char buf[1024];
  while (true) {
    auto ok = co_await stream.read(buf);
    auto len = ok.value();
    fastlog::console.info("  read: {}, len: {}", buf, len);
    ok = co_await stream.write({buf, len});
  }
  fastlog::console.info("  stream closed");
  co_return;
}

faio::task<void> server(uint16_t port) {
  auto addr = faio::net::address::parse("0.0.0.0", port);
  if (!addr) {
    fastlog::console.error("  parse address failed");
    co_return;
  }

  auto has_listener = faio::net::TcpListener::bind(addr.value());
  if (!has_listener) {
    fastlog::console.error("  bind failed: {}", has_listener.error().message());
    co_return;
  }

  fastlog::console.info("  echo server listening on 0.0.0.0:{}", port);
  auto listener = std::move(has_listener.value());
  while (true) {
    auto has_stream = co_await listener.accept();

    if (has_stream) {
      auto &[stream, peer_addr] = has_stream.value();
      fastlog::console.info("  accept a connection from {}", peer_addr);
      faio::spawn(echo(std::move(stream)));
    } else {
      fastlog::console.error("  accept failed: {}",
                             has_stream.error().message());
      co_return;
    }
  }
  co_return;
}

int main() {
  fastlog::set_consolelog_level(fastlog::LogLevel::Info);
  fastlog::console.info("===== 示例: TCP echo server =====");
  auto config = faio::ConfigBuilder().set_num_workers(4).build();
  faio::runtime_context ctx(config);
  faio::block_on(ctx, server(8080));
}
