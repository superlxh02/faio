#include "faio/faio.hpp"
#include "faio/log.hpp"
#include <string_view>

// ============================================================================
// 示例: TCP echo server
// ============================================================================

auto echo(faio::net::TcpStream stream) -> faio::task<void> {
  char buf[1024];
  while (true) {
    auto result = co_await stream.read(buf);
    if (!result) {
      faio::log::logger()->debug("tcp read failed: {}", result.error().message());
      break;
    }
    const auto len = result.value();
    if (len == 0) break;
    faio::log::logger()->trace("tcp received {} bytes: {}", len,
                              std::string_view{buf, len});
    auto written = co_await stream.write_all({buf, len});
    if (!written) {
      faio::log::logger()->debug("tcp write failed: {}", written.error().message());
      break;
    }
  }
  faio::log::logger()->debug("tcp stream closed");
  co_return;
}

faio::task<void> server(uint16_t port) {
  auto addr = faio::net::address::parse("0.0.0.0", port);
  if (!addr) {
    faio::log::logger()->error("  parse address failed");
    co_return;
  }

  auto has_listener = faio::net::TcpListener::bind(addr.value());
  if (!has_listener) {
    faio::log::logger()->error("  bind failed: {}", has_listener.error().message());
    co_return;
  }

  faio::log::logger()->info("  echo server listening on 0.0.0.0:{}", port);
  auto listener = std::move(has_listener.value());
  while (true) {
    auto has_stream = co_await listener.accept();

    if (has_stream) {
      auto &[stream, peer_addr] = has_stream.value();
      faio::log::logger()->debug("accepted TCP connection from {}",
                                peer_addr.to_string());
      faio::spawn_detached(echo(std::move(stream)));
    } else {
      faio::log::logger()->error("  accept failed: {}",
                             has_stream.error().message());
      co_return;
    }
  }
  co_return;
}

int main() {
  faio::log::logger()->set_level(spdlog::level::info);
  faio::log::logger()->info("===== 示例: TCP echo server =====");
  auto config = faio::ConfigBuilder().set_num_workers(4).build();
  faio::runtime::configure(config);
  faio::block_on(server(8080));
}
