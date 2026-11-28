/**
 * @file echo.cpp
 * @brief 使用经过证书和主机名验证的 TLS 连接完成一次本机回显。
 * @details 服务端身份由证书链和私钥提供，客户端从独立的 CA 文件加载信任根，
 * 并验证证书的 `localhost` 主机名。双方复用同一 TCP 连接完成 TLS 握手、
 * 应用数据读写和安全关闭。
 */
#include <array>
#include <chrono>
#include <exception>
#include <span>
#include <string_view>
#include <utility>

#if defined(FAIO_USE_MODULES) && FAIO_USE_MODULES
import faio;
#else
#include <faio/faio.hpp>
#endif

namespace {
using namespace std::chrono_literals;

constexpr std::string_view message{"hello TLS"};

/** @brief 接受一条 TCP 连接，完成服务端 TLS 握手并回显一次请求。 */
faio::task<bool> serve_once(faio::net::tcp_listener& listener, faio::tls::tls_acceptor acceptor) {
  auto accepted = co_await listener.accept().set_timeout(5s);
  if (!accepted) {
    faio::log::logger()->error("接受 TCP 连接失败：{}", accepted.error().message());
    co_return false;
  }

  auto stream = co_await acceptor.accept(std::move(accepted->first));
  if (!stream) {
    faio::log::logger()->error("服务端 TLS 握手失败：{}", stream.error().message());
    co_return false;
  }

  std::array<char, message.size()> request{};
  const auto read = co_await faio::io::read_exact(*stream, request);
  if (!read) {
    faio::log::logger()->error("读取 TLS 请求失败：{}", read.error().message());
    co_return false;
  }

  const auto written = co_await faio::io::write_all(*stream, request);
  if (!written) {
    faio::log::logger()->error("写回 TLS 明文失败：{}", written.error().message());
    co_return false;
  }

  const auto flushed = co_await stream->flush();
  if (!flushed) {
    faio::log::logger()->error("排空服务端密文失败：{}", flushed.error().message());
    co_return false;
  }

  const auto closed = co_await stream->close();
  if (!closed) {
    faio::log::logger()->error("关闭服务端 TLS 会话失败：{}", closed.error().message());
    co_return false;
  }
  co_return true;
}

/** @brief 连接服务端，验证证书后发送并核对完整明文回显。 */
faio::task<bool> request_echo(faio::net::socket_addr destination,
                              faio::tls::tls_connector connector) {
  auto tcp = co_await faio::net::tcp_stream::connect(destination);
  if (!tcp) {
    faio::log::logger()->error("连接 TCP 服务失败：{}", tcp.error().message());
    co_return false;
  }

  auto name = faio::tls::server_name::parse("localhost");
  if (!name) {
    faio::log::logger()->error("解析 TLS 服务端名称失败：{}", name.error().message());
    co_return false;
  }

  auto stream = co_await connector.connect(std::move(*name), std::move(*tcp));
  if (!stream) {
    faio::log::logger()->error("客户端 TLS 握手失败：{}", stream.error().message());
    co_return false;
  }

  const auto written = co_await faio::io::write_all(*stream, message);
  if (!written) {
    faio::log::logger()->error("发送 TLS 明文失败：{}", written.error().message());
    co_return false;
  }

  const auto flushed = co_await stream->flush();
  if (!flushed) {
    faio::log::logger()->error("排空客户端密文失败：{}", flushed.error().message());
    co_return false;
  }

  std::array<char, message.size()> response{};
  const auto read = co_await faio::io::read_exact(*stream, response);
  if (!read) {
    faio::log::logger()->error("读取 TLS 回显失败：{}", read.error().message());
    co_return false;
  }
  if (std::string_view{response.data(), response.size()} != message) {
    faio::log::logger()->error("TLS 回显内容不匹配");
    co_return false;
  }

  const auto closed = co_await stream->close();
  if (!closed) {
    faio::log::logger()->error("关闭客户端 TLS 会话失败：{}", closed.error().message());
    co_return false;
  }
  co_return true;
}

/** @brief 监听本地临时端口，并发运行一次 TLS 回显的客户端与服务端。 */
faio::task<bool> run_echo(faio::tls::tls_acceptor acceptor, faio::tls::tls_connector connector) {
  const faio::net::socket_addr loopback{faio::net::ipv4_addr{127, 0, 0, 1}, 0};
  auto listener = faio::net::tcp_listener::bind(loopback);
  if (!listener) {
    faio::log::logger()->error("绑定 TLS 监听器失败：{}", listener.error().message());
    co_return false;
  }

  const auto destination = listener->local_addr();
  if (!destination) {
    faio::log::logger()->error("查询 TLS 监听地址失败：{}", destination.error().message());
    co_return false;
  }

  auto [server_ok, client_ok] = co_await faio::join(
      serve_once(*listener, std::move(acceptor)), request_echo(*destination, std::move(connector)));

  const auto closed = co_await listener->close();
  if (!closed) {
    faio::log::logger()->error("关闭 TLS 监听器失败：{}", closed.error().message());
  }
  co_return server_ok && client_ok && static_cast<bool>(closed);
}
}  // namespace

/** @brief 从 PEM 文件加载身份和信任根，执行一次 TLS 回显。 */
int main(int argc, char** argv) {
  if (argc != 4) {
    faio::log::logger()->error("用法：{} CERT.pem KEY.pem CA.pem", argv[0]);
    return 2;
  }

  try {
    auto identity = faio::tls::identity::from_pem_files(argv[1], argv[2]);
    if (!identity) {
      faio::log::logger()->error("加载服务端证书和私钥失败：{}", identity.error().message());
      return 1;
    }

    auto roots = faio::tls::trust_store::from_pem_file(argv[3]);
    if (!roots) {
      faio::log::logger()->error("加载客户端 CA 失败：{}", roots.error().message());
      return 1;
    }

    auto server = faio::tls::server_config::builder(std::move(*identity)).build();
    if (!server) {
      faio::log::logger()->error("创建 TLS 服务端配置失败：{}", server.error().message());
      return 1;
    }

    auto client = faio::tls::client_config::builder(std::move(*roots)).build();
    if (!client) {
      faio::log::logger()->error("创建 TLS 客户端配置失败：{}", client.error().message());
      return 1;
    }

    const bool ok = faio::block_on(run_echo(faio::tls::tls_acceptor{std::move(*server)},
                                            faio::tls::tls_connector{std::move(*client)}));
    if (!ok) {
      return 1;
    }

    faio::log::logger()->info("TLS 回显（已验证 localhost）：{}", message);
    return 0;
  } catch (const std::exception& error) {
    faio::log::logger()->error("{}", error.what());
    return 1;
  }
}
