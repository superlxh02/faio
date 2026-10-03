#include "faio/faio.hpp"
#include "faio/log.hpp"

#include <array>
#include <cstdlib>
#include <string>

namespace {
struct TcpBenchmarkConfig {
  std::string host = "0.0.0.0";
  uint16_t port = 18081;
  bool echo = false;
  bool inline_echo_write = false;
};

auto handle_connection(faio::net::TcpStream stream, bool echo, bool inline_echo_write)
    -> faio::task<void> {
  // 公平比较：两种实现都关闭 Nagle，并使用相同的 64 KiB 接收缓冲。
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
      // 诊断模式把完整写循环内联于连接任务，量化 write_all 子协程帧的开销。
      // 仍处理短写和零写，不改变回包字节、套接字设置或每连接请求数量。
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
}  // namespace

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
  // 压测线程数由脚本统一传入，避免各语言默认线程数不同。
  const auto workers = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 4;
  auto builder = faio::ConfigBuilder{}.set_num_workers(workers);
  // 显式诊断配置由统一比较脚本记录；正式验收不传入这些环境变量。
  if (const auto* value = std::getenv("FAIO_BENCH_IO_INTERVAL"))
    builder.set_io_interval(static_cast<uint32_t>(std::stoul(value)));
  if (const auto* value = std::getenv("FAIO_BENCH_IDLE_SPIN_COUNT"))
    builder.set_idle_spin_count(static_cast<uint32_t>(std::stoul(value)));
  // 第六个参数只控制本次服务后端，比较脚本会验证实际选择的名称。
  if (argc > 5) {
    const std::string_view selected{argv[5]};
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
  auto runtime_config = builder.build();
  // 持续就绪任务下的诊断时间预算；未显式设置时保留运行时默认值。
  // 原始参数、环境和源码均由比较脚本留存，不能将扫描结果冒充默认配置验收。
  if (const auto* value = std::getenv("FAIO_BENCH_MAX_IO_DELAY_US"))
    runtime_config._max_io_delay = std::chrono::microseconds(std::stoll(value));
  faio::runtime::configure(runtime_config);
  return faio::block_on(run_server(config));
}
