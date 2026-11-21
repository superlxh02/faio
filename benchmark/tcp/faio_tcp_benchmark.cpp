/**
 * @file benchmark/tcp/faio_tcp_benchmark.cpp
 * @brief faio TCP 回显性能基准。
 */
#include "faio/detail/io/core/context.hpp"
#include "faio/log/logger.hpp"
#include "faio/net/tcp.hpp"
#include "faio/runtime/default.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <array>
#include <cstdlib>
#include <string>

namespace {
/** @brief TCP 基准的监听地址与回显模式配置。 */
struct TcpBenchmarkConfig {
  std::string host = "0.0.0.0";
  uint16_t port = 18081;
  bool echo = false;
  bool inline_echo_write = false;
};

/** @brief 处理基准连接的字节回显或 HTTP 持久响应。 */
auto handle_connection(faio::net::TcpStream stream, bool echo, bool inline_echo_write)
    -> faio::task<void> {
  (void)stream.set_nodelay(true);
  std::array<char, 65536> buf{};
  std::string request_buffer;
  request_buffer.reserve(4096);

  static const std::string response =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "Content-Length: 16\r\n"
      "Connection: keep-alive\r\n"
      "\r\n"
      "hello benchmark\n";

  while (true) {
    auto read_res = co_await stream.read(buf);
    if (!read_res) {
      faio::log::logger()->debug("tcp read failed: {}", read_res.error().message());
      break;
    }

    auto len = read_res.value();
    if (len == 0) {
      break;
    }

    if (echo) {
      if (inline_echo_write) {
        std::size_t offset = 0;
        while (offset < len) {
          auto written =
              co_await stream.write(std::span<const char>(buf.data() + offset, len - offset));
          if (!written || *written == 0)
            co_return;
          offset += *written;
        }
        continue;
      }
      auto written = co_await stream.write_all(std::span<const char>(buf.data(), len));
      if (!written)
        co_return;
      continue;
    }
    if (request_buffer.size() + len > 65536)
      co_return;
    request_buffer.append(buf.data(), len);

    while (true) {
      auto end = request_buffer.find("\r\n\r\n");
      if (end == std::string::npos) {
        break;
      }

      auto write_res =
          co_await stream.write_all(std::span<const char>(response.data(), response.size()));
      if (!write_res) {
        faio::log::logger()->debug("tcp write failed: {}", write_res.error().message());
        co_return;
      }
      request_buffer.erase(0, end + 4);
    }
  }

  co_return;
}

/** @brief 绑定基准端口并提交各连接处理任务。 */
auto run_server(const TcpBenchmarkConfig& config) -> faio::task<int> {
  const auto capabilities = faio::io::io_context::current().domain()->capabilities();
  faio::log::logger()->info("faio tcp benchmark IO backend: {} (native files={})",
                            capabilities.backend,
                            capabilities.native_filesystem);
  faio::log::logger()->flush();
  auto addr_res = faio::net::address::parse(config.host, config.port);
  if (!addr_res) {
    faio::log::logger()->error("parse address failed: {}", addr_res.error().message());
    co_return 1;
  }

  auto listener_res = faio::net::TcpListener::bind(addr_res.value());
  if (!listener_res) {
    faio::log::logger()->error("bind failed: {}", listener_res.error().message());
    co_return 1;
  }

  auto listener = std::move(listener_res.value());
  faio::log::logger()->info("faio tcp benchmark listening on {}:{}", config.host, config.port);

  while (true) {
    auto accept_res = co_await listener.accept();
    if (!accept_res) {
      faio::log::logger()->error("accept failed: {}", accept_res.error().message());
      co_return 1;
    }
    auto [stream, _peer] = std::move(accept_res.value());
    faio::spawn_detached(
        handle_connection(std::move(stream), config.echo, config.inline_echo_write));
  }
}
}

/** @brief 解析参数并启动 faio TCP 回显或 HTTP 基准服务。 */
int main(int argc, char** argv) {
  faio::log::logger()->set_level(spdlog::level::info);

  TcpBenchmarkConfig config;
  if (argc > 1) {
    config.host = argv[1];
  }
  if (argc > 2) {
    config.port = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10));
  }
  config.echo =
      argc > 4
      && (std::string_view(argv[4]) == "echo" || std::string_view(argv[4]) == "echo_inline");
  config.inline_echo_write = argc > 4 && std::string_view(argv[4]) == "echo_inline";
  const auto workers = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 4;
  auto builder = faio::ConfigBuilder{}.set_num_workers(workers);
  if (const auto* value = std::getenv("FAIO_BENCH_IO_INTERVAL"))
    builder.set_io_interval(static_cast<uint32_t>(std::stoul(value)));
  if (const auto* value = std::getenv("FAIO_BENCH_IDLE_SPIN_COUNT"))
    builder.set_idle_spin_count(static_cast<uint32_t>(std::stoul(value)));
  bool current_thread = false;
  int backend_arg = argc;
  for (int index = 5; index < argc; ++index) {
    if (std::string_view{argv[index]} == "--current-thread")
      current_thread = true;
    else if (backend_arg == argc)
      backend_arg = index;
  }
  if (backend_arg < argc) {
    const std::string_view selected{argv[backend_arg]};
#if defined(__linux__)
    if (selected == "--io-backend=epoll")
      builder.set_io_backend(faio::runtime::io_backend::IO_EPOLL);
    else if (selected == "--io-backend=uring")
      builder.set_io_backend(faio::runtime::io_backend::IO_URING);
    else
      throw std::invalid_argument("expected --io-backend=epoll|uring");
#elif defined(_WIN32)
    if (selected != "--io-backend=iocp")
      throw std::invalid_argument("expected --io-backend=iocp");
#else
    (void)selected;
    throw std::invalid_argument("explicit Linux IO backend is unavailable on this platform");
#endif
  }
  if (current_thread) {
    if (workers != 1)
      throw std::invalid_argument("--current-thread requires workers=1");
    builder.set_mode(faio::runtime::mode::current_thread);
  }
  auto runtime_config = builder.build();
  if (const auto* value = std::getenv("FAIO_BENCH_MAX_IO_DELAY_US"))
    runtime_config._max_io_delay = std::chrono::microseconds(std::stoll(value));
  faio::runtime::configure(runtime_config);
  if (current_thread)
    faio::log::logger()->info("faio tcp benchmark runtime: current_thread");
  return faio::block_on(run_server(config));
}
