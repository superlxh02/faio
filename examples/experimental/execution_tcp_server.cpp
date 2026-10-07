// stdexec TCP echo server: exec::task + async_scope, scheduled by faio.
#include <faio/experimental/execution.h>
#include <faio/faio.hpp>
#include <faio/log.hpp>
#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
namespace ex = faio::experimental;
constexpr std::uint16_t default_port = 8081;
constexpr std::size_t buffer_size = 4096;

// faio's I/O awaiters require a native promise. These small tasks provide the
// as_sender bridge; the server and connection workflows below use exec::task.
faio::task<std::pair<faio::net::TcpStream, faio::net::address>>
accept_one(faio::net::TcpListener& listener) {
  auto result = co_await listener.accept();
  if (!result)
    throw std::runtime_error("TCP accept 失败：" + std::string{result.error().message()});
  co_return std::move(*result);
}

faio::task<std::size_t> read_some(faio::net::TcpStream& stream, std::span<char> buffer) {
  const auto result = co_await stream.read(buffer);
  if (!result)
    throw std::runtime_error("TCP 读取失败：" + std::string{result.error().message()});
  co_return *result;
}

faio::task<void> write_all(faio::net::TcpStream& stream, std::span<const char> bytes) {
  const auto result = co_await stream.write_all(bytes);
  if (!result)
    throw std::runtime_error("TCP 写入失败：" + std::string{result.error().message()});
}

exec::task<void> echo_connection(ex::runtime_ref runtime, faio::net::TcpStream stream) {
  // stream and buffer belong to this coroutine frame until every I/O completes.
  std::array<char, buffer_size> buffer{};
  for (;;) {
    const auto size = co_await runtime.as_sender(read_some(stream, buffer));
    if (size == 0)
      co_return;
    // TCP is a byte stream: echo each received range and handle short writes.
    co_await runtime.as_sender(write_all(stream, std::span<const char>{buffer.data(), size}));
  }
}

void report_connection_error(std::exception_ptr error) noexcept {
  // async_scope::spawn needs its error channel handled before reaching the
  // detached receiver. A failed client does not terminate the accept loop.
  try {
    std::rethrow_exception(error);
  } catch (const std::exception& failure) {
    std::fprintf(stderr, "TCP 连接退出：%s\n", failure.what());
  } catch (...) {
    std::fputs("TCP 连接退出：未知异常\n", stderr);
  }
}

exec::task<void> tcp_server(ex::runtime_ref runtime, exec::async_scope& connections,
                           std::uint16_t port) {
  const faio::net::address address{faio::net::v4addr{0, 0, 0, 0}, port};
  auto bound = faio::net::TcpListener::bind(address);
  if (!bound)
    throw std::runtime_error("TCP 绑定失败：" + std::string{bound.error().message()});
  auto listener = std::move(*bound);
  faio::log::logger()->info("stdexec TCP echo 服务监听 {}，Ctrl+C 结束", address.to_string());
  for (;;) {
    auto [stream, peer] = co_await runtime.as_sender(accept_one(listener));
    faio::log::logger()->info("接受 TCP 连接：{}", peer.to_string());
    // bind supplies faio's scheduler and links the scope stop token to this
    // connection's execution graph. spawn starts it while accept continues.
    connections.spawn(stdexec::upon_error(
        runtime.bind(echo_connection(runtime, std::move(stream))), report_connection_error));
  }
}

std::uint16_t parse_port(int argc, char** argv) {
  if (argc == 1)
    return default_port;
  if (argc != 2)
    throw std::invalid_argument("用法：execution_tcp_server [port]，默认端口 8081");
  const std::string_view input{argv[1]};
  std::uint16_t port{};
  const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), port);
  if (error != std::errc{} || end != input.data() + input.size() || port == 0)
    throw std::invalid_argument("端口必须是 1 到 65535 的整数");
  return port;
}
} // namespace

int main(int argc, char** argv) {
  try {
    const auto port = parse_port(argc, argv);
    ex::multi_thread_runtime runtime{ex::runtime_options{
        .mode = faio::runtime::mode::multi_thread,
        .workers = 4,
        .io_backend = faio::runtime::io_backend::IO_EPOLL}};
    // Owner and scope outlive all coroutine frames. Only faio supplies workers;
    // no stdexec thread pool or synchronous run is used on a runtime worker.
    exec::async_scope connections;
    std::exception_ptr error;
    try {
      runtime.run(tcp_server(runtime.ref(), connections, port));
    } catch (...) {
      error = std::current_exception();
    }
    // An accept/bind failure cancels existing clients and drains their I/O
    // before destroying the scope or shutting down the owning runtime.
    connections.request_stop();
    runtime.run(connections.on_empty());
    runtime.shutdown();
    if (error)
      std::rethrow_exception(error);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "stdexec TCP 服务失败：%s\n", error.what());
    return EXIT_FAILURE;
  }
}
