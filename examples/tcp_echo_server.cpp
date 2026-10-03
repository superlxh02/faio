/**
 * 多线程 TCP echo server：一连接一协程。
 * 本文件完整定义监听、连接处理和 main，复制本 cpp 即可阅读全部示例逻辑。
 * 运行后用 nc 127.0.0.1 8080 连接，输入文本即可收到原样回显。
 */
#include "faio/faio.hpp"
#include "faio/log.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
// 每个连接一个协程。stream 按值接收并移入协程帧，连接在任务结束时自动关闭。
faio::task<void> echo_connection(faio::net::TcpStream stream) {
  try {
    std::array<char, 4096> buffer{};
    for (;;) {
      // read 无数据时挂起协程，调度线程可以继续接受新连接或处理其他连接。
      const auto read = co_await stream.read(buffer);
      if (!read) {
        faio::log::logger()->warn("TCP 读取失败：{}", read.error().message());
        co_return;
      }
      if (*read == 0) {
        // TCP 返回 0 表示对端关闭写方向（EOF）；这不是 UDP 的空数据报。
        faio::log::logger()->info("TCP 客户端结束发送，连接退出");
        co_return;
      }
      // TCP 是字节流，单次 read 不代表一条消息。echo 按收到的字节原样回传即可。
      // write 可能短写；write_all 会继续写，直到本次数据全部发送或报告错误。
      const auto written = co_await stream.write_all(
          std::span<const char>{buffer.data(), *read});
      if (!written) {
        faio::log::logger()->warn("TCP 写入失败：{}",
                                  written.error().message());
        co_return;
      }
    }
  } catch (const std::exception &error) {
    // 此函数由 spawn_detached 运行，必须在任务体内处理异常。
    // 单个客户端失败只结束自己的连接，不让异常逃出并终止整个进程。
    faio::log::logger()->warn("TCP 连接退出：{}", error.what());
  }
}

faio::task<void> example_tcp_echo_server(std::uint16_t port) {
  const faio::net::address address{faio::net::v4addr{127, 0, 0, 1}, port};
  auto bound = faio::net::TcpListener::bind(address);
  if (!bound)
    throw std::runtime_error("TCP 绑定失败：" +
                             std::string{bound.error().message()});
  auto listener = std::move(*bound);
  faio::log::logger()->info("TCP echo 服务监听 {}，Ctrl+C 结束",
                            address.to_string());
  for (;;) {
    // accept 等待新连接时挂起；不需要每个连接分配一个操作系统线程。
    auto accepted = co_await listener.accept();
    if (!accepted)
      throw std::runtime_error("TCP accept 失败：" +
                               std::string{accepted.error().message()});
    auto &[stream, peer] = *accepted;
    faio::log::logger()->info("接受 TCP 连接：{}", peer.to_string());
    // move 明确移交连接所有权；后台连接协程和下一次 accept 并发运行。
    faio::spawn_detached(echo_connection(std::move(stream)));
  }
}
} // namespace

int main(int argc, char **argv) {
  try {
    // 后端参数在本文件内处理，示例不依赖其他 example 的工具函数。
    auto builder = faio::config_builder{};
#if defined(__linux__)
    if (argc > 2)
      throw std::invalid_argument("仅支持 --io-backend=epoll|uring");
    std::string_view selection;
    if (argc == 2) {
      constexpr std::string_view prefix{"--io-backend="};
      const std::string_view argument{argv[1]};
      if (!argument.starts_with(prefix) || argument.size() == prefix.size())
        throw std::invalid_argument("请使用 --io-backend=epoll|uring");
      selection = argument.substr(prefix.size());
    } else if (const char *environment = std::getenv("FAIO_TEST_IO_BACKEND")) {
      selection = environment;
    }
    if (selection == "epoll")
      builder.set_io_backend(faio::runtime::io_backend::IO_EPOLL);
    else if (selection == "uring")
      builder.set_io_backend(faio::runtime::io_backend::IO_URING);
    else if (!selection.empty())
      throw std::invalid_argument("IO 后端必须是 epoll 或 uring");
#else
    (void)argv;
    if (argc > 1)
      throw std::invalid_argument(
          "本平台使用固定 IO 后端，无需选择 Linux 后端");
#endif
    // 默认就是 multi_thread，这里显式写出配置方便与
    // tcp_echo_server_single_thread.cpp 对照。
    faio::runtime::configure(builder.set_mode(faio::runtime::mode::multi_thread)
                                 .set_num_workers(4)
                                 .build());
    faio::block_on(example_tcp_echo_server(8080));
    faio::runtime::shutdown();
  } catch (const std::exception &error) {
    faio::log::logger()->error("TCP 服务失败：{}", error.what());
    return 1;
  }
}
