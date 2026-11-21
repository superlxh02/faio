/**
 * @file udp_echo.cpp
 * @brief 绑定本机 UDP 套接字，按来源地址回显一个数据报。
 * @details 两个端点均使用系统分配的端口。发送和接收各处理一个完整数据报，
 * 接收端通过 `recv_from` 携带的来源地址决定回复目的地。
 */
#include <faio/faio.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace std::chrono_literals;

constexpr std::string_view message{"hello UDP"};

/**
 * @brief 提取网络操作的成功值，失败时补充步骤名称。
 * @param result 要检查的网络操作结果。
 * @param operation 当前步骤的名称。
 * @return 操作的地址、套接字或字节数等成功值。
 */
template <class T>
T checked(faio::expected<T> result, std::string_view operation) {
  if (!result)
    throw std::runtime_error(std::string{operation} + "：" + std::string{result.error().message()});
  return std::move(result).value();
}

/**
 * @brief 发送一个数据报，使用接收方观测到的来源地址回显，然后核对回包。
 * @details 消息长度始终小于接收缓冲区。UDP 不提供交付保证，等待回包时设置期限，
 * 以便没有响应时结束示例。收到长度为零的数据报仍代表成功收到一条消息。
 */
faio::task<void> run_echo() {
  const faio::net::SocketAddr loopback{faio::net::Ipv4Addr{127, 0, 0, 1}, 0};
  auto server = checked(faio::net::UdpSocket::bind(loopback), "绑定 UDP 服务端");
  auto client = checked(faio::net::UdpSocket::bind(loopback), "绑定 UDP 客户端");
  const auto destination = checked(server.local_addr(), "查询 UDP 服务地址");

  const auto sent = checked(co_await client.send_to(std::span<const char>{message}, destination),
                            "发送 UDP 数据报");
  if (sent != message.size())
    throw std::runtime_error("UDP 数据报未完整发送");

  /** @brief recv_from 同时交付字节数和实际来源，回显只发送本次收到的有效字节。 */
  std::array<char, 64> request{};
  const auto [size, peer] =
      checked(co_await server.recv_from(request).set_timeout(5s), "接收 UDP 请求");
  const auto echoed = checked(
      co_await server.send_to(std::span<const char>{request}.first(size), peer), "写回 UDP 数据报");
  if (echoed != size)
    throw std::runtime_error("UDP 数据报未完整写回");

  std::array<char, 64> response{};
  const auto [reply_size, source] =
      checked(co_await client.recv_from(response).set_timeout(5s), "接收 UDP 响应");
  if (source != destination || std::string_view{response.data(), reply_size} != message)
    throw std::runtime_error("UDP 回显来源或内容不匹配");
}
}  // namespace

/** @brief 验证一个本机 UDP 数据报的回显，成功返回 0，失败返回 1。 */
int main() {
  try {
    faio::block_on(run_echo());
    std::cout << "UDP 回显：" << message << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
