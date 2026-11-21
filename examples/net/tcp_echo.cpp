/**
 * @file tcp_echo.cpp
 * @brief 在本机建立一条 TCP 连接并验证完整字节回显。
 * @details 监听端口由系统分配，客户端和服务端均在本程序内运行。双方约定一条
 * 固定长度的消息，用 `read_exact` 和 `write_all` 处理字节流的短读和短写。
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

constexpr std::string_view message{"hello TCP"};

/**
 * @brief 提取操作结果，失败时附上操作名称并交给入口统一报告。
 * @param result 成功值或 faio 错误。移动所有权的操作会返回可移动对象。
 * @param operation 当前步骤的名称。
 * @return 成功值，或者在 `expected<void>` 成功时直接返回。
 */
template <class T>
T checked(faio::expected<T> result, std::string_view operation) {
  if (!result)
    throw std::runtime_error(std::string{operation} + "：" + std::string{result.error().message()});
  return std::move(result).value();
}

/**
 * @brief 接受一条连接，读完约定长度的消息后原样写回。
 * @param listener 主任务持有的监听器，存活到 `join` 返回。
 * @details TCP 没有消息边界。生产协议通常先解析长度或分隔符，本例双方已约定长度。
 */
faio::task<void> serve_once(faio::net::TcpListener& listener) {
  auto [stream, peer] = checked(co_await listener.accept().set_timeout(5s), "接受 TCP 连接");
  std::array<char, message.size()> bytes{};
  checked(co_await stream.read_exact(bytes), "读取请求");
  checked(co_await stream.write_all(bytes), "写回响应");
  checked(co_await stream.shutdown(faio::io::ShutdownBehavior::Write), "关闭服务端写方向");
}

/**
 * @brief 向监听地址发送一条消息，核对响应并检查对端结束发送。
 * @param destination 系统为监听器分配的本地地址和端口。
 */
faio::task<void> request_echo(faio::net::SocketAddr destination) {
  auto stream = checked(co_await faio::net::TcpStream::connect(destination), "连接 TCP 服务");
  checked(co_await stream.write_all(std::span<const char>{message}), "发送请求");

  std::array<char, message.size()> response{};
  checked(co_await stream.read_exact(response), "读取响应");
  if (std::string_view{response.data(), response.size()} != message)
    throw std::runtime_error("TCP 回显内容不匹配");

  /** @brief 使用非空缓冲读取，返回 0 表示对端写方向已关闭，且没有更多字节。 */
  std::array<char, 1> tail{};
  if (checked(co_await stream.read(tail).set_timeout(5s), "确认响应结束") != 0)
    throw std::runtime_error("TCP 回显包含多余字节");
}

/** @brief 建立监听，并用 `join` 等待一次回显的客户端与服务端共同结束。 */
faio::task<void> run_echo() {
  const faio::net::SocketAddr loopback{faio::net::Ipv4Addr{127, 0, 0, 1}, 0};
  auto listener = checked(faio::net::TcpListener::bind(loopback), "绑定 TCP 监听器");
  const auto destination = checked(listener.local_addr(), "查询 TCP 监听地址");
  co_await faio::join(serve_once(listener), request_echo(destination));
  checked(co_await listener.close(), "关闭 TCP 监听器");
}
}  // namespace

/** @brief 验证一次本机 TCP 回显，成功返回 0，失败返回 1。 */
int main() {
  try {
    faio::block_on(run_echo());
    std::cout << "TCP 回显：" << message << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
