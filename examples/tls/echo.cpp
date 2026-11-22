/**
 * @file echo.cpp
 * @brief 使用经过证书和主机名验证的 TLS 连接完成一次本机回显。
 * @details 服务端身份由证书链和私钥提供，客户端从独立的 CA 文件加载信任根，
 * 并验证证书的 `localhost` 主机名。双方复用同一 TCP 连接完成 TLS 握手、
 * 应用数据读写和安全关闭。
 */
#if defined(FAIO_USE_MODULES) && FAIO_USE_MODULES
import faio;
import faio.tls;
#else
#include <faio/faio.hpp>
#endif

#include <array>
#include <coroutine>
#include <chrono>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace std::chrono_literals;

constexpr std::string_view message{"hello TLS"};

/**
 * @brief 提取操作结果，失败时保留握手、读写或关闭的步骤名称。
 * @param result 成功值或 faio 错误；TLS 握手成功后，结果持有安全字节流。
 * @param operation 当前步骤的名称。
 * @return 操作的成功值，或者在 `expected<void>` 成功时直接返回。
 */
template <class T>
T checked(faio::expected<T> result, std::string_view operation) {
  if (!result)
    throw std::runtime_error(std::string{operation} + "：" + std::string{result.error().message()});
  return std::move(result).value();
}

/**
 * @brief 接受一条 TCP 连接，将其交给服务端 TLS 握手器，并回显固定长度的消息。
 * @param listener 主任务持有的监听器，存活到 `join` 返回。
 * @param acceptor 持有证书和私钥的服务端握手器。
 */
faio::task<void> serve_once(faio::net::TcpListener& listener, faio::tls::TlsAcceptor acceptor) {
  auto [tcp, peer] = checked(co_await listener.accept().set_timeout(5s), "接受 TCP 连接");
  auto stream = checked(co_await acceptor.accept(std::move(tcp)), "完成服务端 TLS 握手");

  std::array<char, message.size()> request{};
  checked(co_await faio::io::read_exact(stream, request), "读取 TLS 请求");
  checked(co_await faio::io::write_all(stream, request), "写回 TLS 明文");
  checked(co_await stream.flush(), "排空服务端 TLS 待发送密文");
  checked(co_await stream.close(), "关闭服务端 TLS 会话");
}

/**
 * @brief 用显式信任根和 `localhost` 验证服务端，发送并核对完整明文回显。
 * @param destination TCP 连接使用的数字地址，证书身份则使用独立的 `localhost` 名称。
 * @param connector 持有信任根的客户端握手器。
 */
faio::task<void> request_echo(faio::net::SocketAddr destination,
                              faio::tls::TlsConnector connector) {
  auto tcp = checked(co_await faio::net::TcpStream::connect(destination), "连接 TCP 服务");
  auto name = checked(faio::tls::ServerName::parse("localhost"), "解析 TLS 服务端名称");
  auto stream = checked(co_await connector.connect(std::move(name), std::move(tcp)),
                        "完成客户端 TLS 握手和证书验证");

  /** @brief write_all 处理短写。flush 等待明文生成的密文排空，随后才能等对端响应。 */
  checked(co_await faio::io::write_all(stream, message), "发送 TLS 明文");
  checked(co_await stream.flush(), "排空客户端 TLS 待发送密文");
  std::array<char, message.size()> response{};
  checked(co_await faio::io::read_exact(stream, response), "读取 TLS 回显");
  if (std::string_view{response.data(), response.size()} != message)
    throw std::runtime_error("TLS 回显内容不匹配");

  /** @brief 双方分别调用 close，交换 close_notify 后由 TLS 流关闭底层 TCP 连接。 */
  checked(co_await stream.close(), "关闭客户端 TLS 会话");
}

/**
 * @brief 监听本地临时端口，并发运行只处理一次请求的客户端与服务端。
 * @param acceptor 已装载服务端身份的握手器。
 * @param connector 已装载客户端信任根的握手器。
 */
faio::task<void> run_echo(faio::tls::TlsAcceptor acceptor, faio::tls::TlsConnector connector) {
  const faio::net::SocketAddr loopback{faio::net::Ipv4Addr{127, 0, 0, 1}, 0};
  auto listener = checked(faio::net::TcpListener::bind(loopback), "绑定 TLS 监听器");
  const auto destination = checked(listener.local_addr(), "查询 TLS 监听地址");
  /** @brief join 按参数顺序传播失败，优先报告客户端的证书验证错误。 */
  co_await faio::join(request_echo(destination, std::move(connector)),
                      serve_once(listener, std::move(acceptor)));
  checked(co_await listener.close(), "关闭 TLS 监听器");
}
}  // namespace

/**
 * @brief 从 PEM 文件加载身份和信任根，执行一次 TLS 回显。
 * @param argc 包括程序名在内的参数数目，应为 4。
 * @param argv 依次包含程序名、服务端证书链、服务端私钥和客户端信任的 CA 文件。
 * @return 成功返回 0，证书配置或连接失败返回 1，参数个数错误返回 2。
 */
int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "用法：" << argv[0] << " CERT.pem KEY.pem CA.pem\n";
    return 2;
  }
  try {
    auto identity =
        checked(faio::tls::Identity::from_pem_files(argv[1], argv[2]), "加载服务端证书和私钥");
    auto roots = checked(faio::tls::TrustStore::from_pem_file(argv[3]), "加载客户端 CA");
    auto server = checked(faio::tls::ServerConfig::builder(std::move(identity)).build(),
                          "创建 TLS 服务端配置");
    auto client =
        checked(faio::tls::ClientConfig::builder(std::move(roots)).build(), "创建 TLS 客户端配置");

    faio::block_on(run_echo(faio::tls::TlsAcceptor{std::move(server)},
                            faio::tls::TlsConnector{std::move(client)}));
    std::cout << "TLS 回显（已验证 localhost）：" << message << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
