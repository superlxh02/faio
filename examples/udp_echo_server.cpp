/**
 * 简单 UDP 服务：把每个收到的数据报原样发送回来源地址。
 * 运行后用 nc -u 127.0.0.1 9090 发送文本；Ctrl+C 结束服务。
 */
#include "faio/faio.hpp"
#include "faio/log.hpp"
#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
faio::task<void> example_udp_echo_server() {
  const faio::net::address address{faio::net::v4addr{127, 0, 0, 1}, 9090};
  auto bound = faio::net::UdpDatagram::bind(address);
  if (!bound)
    throw std::runtime_error("UDP 绑定失败：" +
                             std::string{bound.error().message()});
  auto socket = std::move(*bound);
  // 回显最大 IPv4 数据报还需要足够的发送缓冲；macOS 默认只有 9216 字节。
  // 这里只配置本示例的套接字，不修改系统参数；失败必须明确报告，不能静默丢包。
  const auto send_buffer = socket.set_send_buffer_size(128 * 1024);
  if (!send_buffer)
    throw std::runtime_error("UDP 发送缓冲配置失败：" +
                             std::string{send_buffer.error().message()});
  // UDP 单个数据报可以很大，缓冲区覆盖 IPv4 UDP 的最大有效载荷，避免静默截断。
  std::array<char, 65507> buffer{};
  faio::log::logger()->info("UDP echo 服务监听 {}，Ctrl+C 结束",
                            address.to_string());
  for (;;) {
    // recv_from 没有数据时挂起；结果包含长度和发送者地址，UDP 不必先 accept。
    auto received = co_await socket.recv_from(buffer);
    if (!received)
      throw std::runtime_error("UDP 接收失败：" +
                               std::string{received.error().message()});
    const auto &[bytes, peer] = *received;
    // UDP 保留消息边界。bytes == 0 是合法空数据报，仍然应回复，不能当 EOF。
    const auto sent = co_await socket.send_to(
        std::span<const char>{buffer.data(), bytes}, peer);
    if (!sent)
      throw std::runtime_error("UDP 发送失败：" +
                               std::string{sent.error().message()});
    if (*sent != bytes)
      throw std::runtime_error("UDP 数据报未完整发送");
    faio::log::logger()->info("UDP 回显 {} 字节，目标 {}", bytes,
                              peer.to_string());
    // echo 不维护客户端状态，也不需要为每个数据报启动独立协程。
    // UDP 本身不保证送达、顺序或去重；此处仅展示收发和消息边界。
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
    faio::runtime::configure(builder.set_num_workers(2).build());
    faio::block_on(example_udp_echo_server());
    faio::runtime::shutdown();
  } catch (const std::exception &error) {
    faio::log::logger()->error("UDP 服务失败：{}", error.what());
    return 1;
  }
}
