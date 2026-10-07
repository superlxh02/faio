#include <faio/faio.hpp>
#include <chrono>
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include <faio/experimental/execution.h>
#endif
#include <faio/experimental/async_main.h>
#include <faio/log.hpp>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>

[[= faio::experimental::main(tcp_echo_server)]][[= faio::experimental::runtime_options{
    .mode = faio::runtime::mode::multi_thread,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]] auto tcp_echo_server(int argc, char** argv)
    -> faio::task<int> {
  const faio::net::address address{faio::net::v4addr{127, 0, 0, 1}, 8080};
  auto bound = faio::net::TcpListener::bind(address);
  if (!bound) {
    faio::log::logger()->error("TCP 绑定失败：" + std::string{bound.error().message()});
    co_return -1;
  }
  auto listener = std::move(*bound);
  faio::log::logger()->info("TCP echo 服务监听 {}，Ctrl+C 结束", address.to_string());
  for (;;) {
    auto accepted = co_await listener.accept();
    if (!accepted) {
      faio::log::logger()->error("TCP accept 失败：" + std::string{accepted.error().message()});
      co_return -1;
    }
    auto& [stream, peer] = *accepted;
    faio::log::logger()->info("接受 TCP 连接：{}", peer.to_string());
    faio::spawn_detached([](faio::net::TcpStream stream) -> faio::task<void> {
      std::array<char, 4096> buffer{};
      for (;;) {
        const auto read = co_await stream.read(buffer);
        if (!read) {
          faio::log::logger()->warn("TCP 读取失败：{}", read.error().message());
          co_return;
        }
        if (*read == 0) {
          faio::log::logger()->info("TCP 客户端结束发送，连接退出");
          co_return;
        }
        const auto written = co_await stream.write_all(std::span<const char>{buffer.data(), *read});
        if (!written) {
          faio::log::logger()->warn("TCP 写入失败：{}", written.error().message());
          co_return;
        }
      }
    }(std::move(stream)));
  }
}
