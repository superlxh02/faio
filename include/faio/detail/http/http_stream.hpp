#ifndef FAIO_DETAIL_HTTP_HTTP_STREAM_HPP
#define FAIO_DETAIL_HTTP_HTTP_STREAM_HPP

#include <memory>
#include "faio/detail/http/v2/client_session_v2.hpp"
#include "faio/detail/http/v1/client_session_v1.hpp"
#include "faio/detail/http/types.hpp"
#include "faio/detail/common/error.hpp"

namespace faio::detail::http {

enum class HttpProtocol {
  Auto,
  Http1,
  Http2,
};

// 对外的 HTTP 客户端封装。
//
// 职责：
// - 屏蔽 HTTP/1.1 与 HTTP/2 客户端会话差异，对外提供统一 request 接口。
// - 在 Auto 模式下优先尝试 HTTP/2，失败后自动回退到 HTTP/1.1。
class HttpStream {
public:
  HttpStream() = default;

    explicit HttpStream(std::unique_ptr<Http2ClientSession> session)
      : _h2_session(std::move(session)), _protocol(HttpProtocol::Http2) {}

    explicit HttpStream(std::unique_ptr<Http1ClientSession> session)
      : _h1_session(std::move(session)), _protocol(HttpProtocol::Http1) {}

  ~HttpStream() = default;

  HttpStream(HttpStream&&) = default;
  HttpStream& operator=(HttpStream&&) = default;

  // 按 host + port 建连，默认 Auto 协议协商策略。
  static auto connect(const std::string& host, uint16_t port)
      -> task<expected<HttpStream>> {
    co_return co_await connect(host, port, HttpProtocol::Auto);
  }

  // 按 host + port 建连（显式指定协议）。
  static auto connect(const std::string& host, uint16_t port,
                      HttpProtocol protocol)
      -> task<expected<HttpStream>> {
    // 先把 host 解析成 socket 地址
    auto addr_res = net::detail::SocketAddr::parse(host, port);
    if (!addr_res) {
      co_return std::unexpected(addr_res.error());
    }
    co_return co_await connect(addr_res.value(), protocol);
  }

  // 按地址对象建连（Auto 协议）。
  static auto connect(const net::detail::SocketAddr& addr)
      -> task<expected<HttpStream>> {
    co_return co_await connect(addr, HttpProtocol::Auto);
  }

  // 按地址对象建连（指定协议）。
  static auto connect(const net::detail::SocketAddr& addr, HttpProtocol protocol)
      -> task<expected<HttpStream>> {
    auto connect_h1 = [&addr]() -> task<expected<HttpStream>> {
      auto stream_res = co_await net::detail::TcpStream::connect(addr);
      if (!stream_res) {
        co_return std::unexpected(stream_res.error());
      }
        auto session_ptr = std::make_unique<Http1ClientSession>(
          std::move(stream_res.value()));
      auto init_res = co_await session_ptr->initialize();
      if (!init_res) {
        co_return std::unexpected(init_res.error());
      }
      co_return HttpStream(std::move(session_ptr));
    };

    auto connect_h2 = [&addr]() -> task<expected<HttpStream>> {
      auto stream_res = co_await net::detail::TcpStream::connect(addr);
      if (!stream_res) {
        co_return std::unexpected(stream_res.error());
      }
        auto session_ptr = std::make_unique<Http2ClientSession>(
          std::move(stream_res.value()));
      auto init_res = co_await session_ptr->initialize();
      if (!init_res) {
        co_return std::unexpected(init_res.error());
      }
      co_return HttpStream(std::move(session_ptr));
    };

    if (protocol == HttpProtocol::Http1) {
      co_return co_await connect_h1();
    }
    if (protocol == HttpProtocol::Http2) {
      co_return co_await connect_h2();
    }

    // Auto：先尝试 HTTP/2，再回退 HTTP/1.1。
    auto h2_res = co_await connect_h2();
    if (h2_res) {
      co_return h2_res;
    }

    co_return co_await connect_h1();
  }

  // 发起一个请求并等待完整响应。
  // 根据当前 _protocol 路由到对应会话实现。
  auto request(const HttpRequest& req)
      -> task<expected<HttpResponse>> {
    if (_protocol == HttpProtocol::Http2 && _h2_session) {
      co_return co_await _h2_session->request(req);
    }
    if (_protocol == HttpProtocol::Http1 && _h1_session) {
      co_return co_await _h1_session->request(req);
    }

    if (!_h1_session && !_h2_session) {
      co_return std::unexpected(make_error(static_cast<int>(Error::Http2Internal)));
    }

    co_return std::unexpected(make_error(static_cast<int>(Error::Http2Internal)));
  }

  // 关闭连接。
  // 调用方需要 co_await 本接口，确保底层关闭 IO 完成。
  auto close() -> task<void> {
    if (_h2_session) {
      co_await _h2_session->close();
      co_return;
    }
    if (_h1_session) {
      co_await _h1_session->close();
    }
  }

private:
  std::unique_ptr<Http2ClientSession> _h2_session;
  std::unique_ptr<Http1ClientSession> _h1_session;
  HttpProtocol _protocol = HttpProtocol::Auto;
};

} // namespace faio::detail::http

#endif // FAIO_DETAIL_HTTP_HTTP_STREAM_HPP
