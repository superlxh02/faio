/**
 * @file unix_echo.cpp
 * @brief 在独立临时目录中完成一次 Unix 域套接字回显。
 * @details 并发运行服务端与客户端，验证消息及 EOF，关闭连接后清理 socket 路径。
 */
#include <faio/faio.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {
using namespace std::chrono_literals;

constexpr std::string_view message{"hello Unix socket"};

/** @brief 接受一条连接，读满请求后回写，再关闭连接。 */
faio::task<void> serve_once(faio::net::unix::unix_listener& listener) {
  auto accepted = (co_await listener.accept().set_timeout(5s)).value();
  auto& stream = accepted.first;
  std::array<char, message.size()> request{};
  (co_await stream.read_exact(request)).value();
  (co_await stream.write_all(request)).value();
  (co_await stream.close()).value();
}

/** @brief 连接本地 socket，发送消息并验证完整回显及服务端关闭。 */
faio::task<void> request_echo(faio::net::unix::unix_addr address) {
  auto stream = (co_await faio::net::unix::unix_stream::connect(address)).value();
  (co_await stream.write_all(std::span<const char>{message})).value();

  std::array<char, message.size()> response{};
  (co_await stream.read_exact(response)).value();
  if (std::string_view{response.data(), response.size()} != message)
    throw std::runtime_error("Unix 域套接字回显内容不匹配");

  char tail{};
  if ((co_await stream.read(std::span<char>{&tail, 1}).set_timeout(5s)).value() != 0)
    throw std::runtime_error("Unix 域套接字回显后预期收到 EOF");
  (co_await stream.close()).value();
}

/** @brief 绑定路径地址，并等待服务端和客户端都结束后关闭监听器。 */
faio::task<void> run_echo(faio::net::unix::unix_addr address) {
  auto listener = faio::net::unix::unix_listener::bind(address).value();
  faio::log::logger()->info("Unix 域套接字监听 {}", address.to_string());
  co_await faio::join(serve_once(listener), request_echo(address));
  (co_await listener.close()).value();
  faio::log::logger()->info("Unix 域套接字回显成功：{}", message);
}
}  // namespace

/** @brief 创建独占且路径足够短的临时目录，演示结束后删除 socket 文件及目录。 */
int main() {
  char directory[]{"/tmp/faio-unix-XXXXXX"};
  if (!::mkdtemp(directory)) {
    const std::error_code error{errno, std::generic_category()};
    faio::log::logger()->error("创建临时目录失败：{}", error.message());
    return 1;
  }

  int result = 0;
  try {
    const auto address =
        faio::net::unix::unix_addr::pathname(std::string{directory} + "/echo.sock").value();
    faio::block_on(run_echo(address));
  } catch (const std::bad_expected_access<faio::error>& error) {
    faio::log::logger()->error("Unix 域套接字示例失败：{}", error.error().message());
    result = 1;
  } catch (const std::exception& error) {
    faio::log::logger()->error("Unix 域套接字示例失败：{}", error.what());
    result = 1;
  }

  std::error_code cleanup_error;
  std::filesystem::remove_all(directory, cleanup_error);
  if (cleanup_error) {
    faio::log::logger()->error("清理 socket 目录失败：{}", cleanup_error.message());
    return 1;
  }
  return result;
}
