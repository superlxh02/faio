#ifndef FAIO_DETAIL_HTTP_SERVER_SESSION_V1_HPP
#define FAIO_DETAIL_HTTP_SERVER_SESSION_V1_HPP

#include "faio/detail/common/error.hpp"
#include "faio/detail/http/types.hpp"
#include "faio/detail/net.hpp"
#include "fastlog/fastlog.hpp"
#include <llhttp.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace faio::detail::http {

using Handler = std::function<task<HttpResponse>(const HttpRequest &)>;

class Http1ServerSession {
public:
  explicit Http1ServerSession(faio::net::detail::TcpStream stream)
      : _stream(std::move(stream)) {
    // 初始化 llhttp 请求解析器并挂接解析回调。
    llhttp_settings_init(&_settings);
    _settings.on_message_begin = &Http1ServerSession::on_message_begin;
    _settings.on_url = &Http1ServerSession::on_url;
    _settings.on_header_field = &Http1ServerSession::on_header_field;
    _settings.on_header_value = &Http1ServerSession::on_header_value;
    _settings.on_headers_complete = &Http1ServerSession::on_headers_complete;
    _settings.on_body = &Http1ServerSession::on_body;
    _settings.on_message_complete = &Http1ServerSession::on_message_complete;

    llhttp_init(&_parser, HTTP_REQUEST, &_settings);
    _parser.data = this;
  }

  Http1ServerSession(const Http1ServerSession &) = delete;
  Http1ServerSession &operator=(const Http1ServerSession &) = delete;
  Http1ServerSession(Http1ServerSession &&) = delete;
  Http1ServerSession &operator=(Http1ServerSession &&) = delete;

  auto run(const Handler &handler,
           std::span<const uint8_t> initial_data = {}) -> task<void> {
    // 主循环：读 socket -> 解析请求 -> 调 handler -> 回写响应。
    fastlog::console.debug("http/1.1 server session loop start, fd={}",
                           _stream.fd());

    if (!initial_data.empty()) {
      // 连接探测阶段已读到首包，这里先交给解析器。
      auto proc_res = process_received_data(initial_data);
      if (!proc_res) {
        fastlog::console.error("http/1.1 process initial data failed: {}",
                               proc_res.error().message());
        co_return;
      }

      // 把首包阶段解析出的请求全部处理完。
      auto flush_res = co_await consume_pending_requests(handler);
      if (!flush_res) {
        fastlog::console.error("http/1.1 flush pending failed: {}",
                               flush_res.error().message());
        co_return;
      }
      if (!_keep_alive) {
        co_return;
      }
    }

    while (true) {
      std::vector<char> buf(4096);
      // 1) 读取网络数据。
      auto read_res = co_await _stream.read(std::span(buf.data(), buf.size()));
      if (!read_res) {
        fastlog::console.debug("http/1.1 read finished: {}",
                               read_res.error().message());
        break;
      }

      auto len = read_res.value();
      if (len == 0) {
        // 2) EOF：通知 llhttp 做收尾解析。
        auto finish_err = llhttp_finish(&_parser);
        if (finish_err != HPE_OK) {
          fastlog::console.warn(
              "http/1.1 finish parse failed: {} reason={}",
              llhttp_errno_name(finish_err),
              llhttp_get_error_reason(&_parser) ? llhttp_get_error_reason(&_parser)
                                                : "");
        }

        // 3) EOF 前可能仍有已解析请求，继续 flush。
        auto flush_res = co_await consume_pending_requests(handler);
        if (!flush_res) {
          fastlog::console.error("http/1.1 flush pending failed on eof: {}",
                                 flush_res.error().message());
        }
        break;
      }

        // 2) 把本次字节流喂给 llhttp。
      auto proc_res = process_received_data(
          std::span(reinterpret_cast<const uint8_t *>(buf.data()), len));
      if (!proc_res) {
        fastlog::console.warn("http/1.1 parse error: {}", proc_res.error().message());
        break;
      }

      // 3) 处理本轮解析产出的所有完整请求。
      auto flush_res = co_await consume_pending_requests(handler);
      if (!flush_res) {
        fastlog::console.error("http/1.1 flush pending failed: {}",
                               flush_res.error().message());
        break;
      }
      if (!_keep_alive) {
        break;
      }
    }

    co_await _stream.close();
    fastlog::console.debug("http/1.1 server session loop end, fd={}", _stream.fd());
  }

private:
  struct PendingRequest {
    HttpRequest request;
    bool keep_alive = true;
  };

  static auto lower_ascii(std::string value) -> std::string {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                     return static_cast<char>(std::tolower(ch));
                   });
    return value;
  }

  static auto trim_copy(std::string value) -> std::string {
    auto not_space = [](unsigned char ch) {
      return ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n';
    };

    auto first = std::find_if(value.begin(), value.end(), not_space);
    auto last = std::find_if(value.rbegin(), value.rend(), not_space).base();
    if (first >= last) {
      return {};
    }
    return std::string(first, last);
  }

  static auto status_reason_phrase(int status) -> std::string_view {
    switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 500:
      return "Internal Server Error";
    default:
      return "";
    }
  }

  auto process_received_data(std::span<const uint8_t> data) -> expected<void> {
    // 把网络层读到的字节流交给 llhttp 增量解析。
    auto err = llhttp_execute(
        &_parser, reinterpret_cast<const char *>(data.data()), data.size());
    if (err != HPE_OK) {
      return std::unexpected(make_error(EPROTO));
    }
    return expected<void>();
  }

  auto consume_pending_requests(const Handler &handler) -> task<expected<void>> {
    // 一个 read 可能解析出多个请求（pipeline），逐条处理。
    while (!_pending_requests.empty()) {
      // 取出一条完整请求。
      auto pending = std::move(_pending_requests.front());
      _pending_requests.pop();

      // 调业务 handler。
      auto response = co_await handler(pending.request);
      // 按请求 keep-alive 语义写回响应。
      auto write_res = co_await write_response(response, pending.keep_alive);
      if (!write_res) {
        co_return std::unexpected(write_res.error());
      }
      _keep_alive = pending.keep_alive;
    }
    co_return expected<void>();
  }

  auto write_response(const HttpResponse &resp, bool keep_alive)
      -> task<expected<void>> {
    // 组装响应头并保证 content-length / connection 语义完整。
    std::string header_block;
    header_block.reserve(512);
    header_block.append("HTTP/1.1 ");
    header_block.append(std::to_string(resp.status()));
    auto reason = status_reason_phrase(resp.status());
    if (!reason.empty()) {
      header_block.push_back(' ');
      header_block.append(reason.data(), reason.size());
    }
    header_block.append("\r\n");

    // 标记关键头是否已显式设置。
    auto has_content_length = false;
    auto has_connection = false;
    for (const auto &[name, value] : resp.headers()) {
      auto lower = lower_ascii(name);
      if (lower == "content-length") {
        has_content_length = true;
      } else if (lower == "connection") {
        has_connection = true;
      }
      header_block.append(name);
      header_block.append(": ");
      header_block.append(value);
      header_block.append("\r\n");
    }

    if (!has_content_length) {
      // 未设置时自动补 content-length。
      header_block.append("content-length: ");
      header_block.append(std::to_string(resp.body().size()));
      header_block.append("\r\n");
    }

    if (!has_connection) {
      // 未设置时按 keep_alive 参数补 connection。
      header_block.append("connection: ");
      header_block.append(keep_alive ? "keep-alive" : "close");
      header_block.append("\r\n");
    }

    header_block.append("\r\n");

    // 先写响应头。
    auto header_write = co_await _stream.write_all(
        std::span<const char>(header_block.data(), header_block.size()));
    if (!header_write) {
      co_return std::unexpected(header_write.error());
    }

    if (!resp.body().empty()) {
      // 再写响应体。
      auto body_write = co_await _stream.write_all(std::span(
          reinterpret_cast<const char *>(resp.body().data()), resp.body().size()));
      if (!body_write) {
        co_return std::unexpected(body_write.error());
      }
    }

    co_return expected<void>();
  }

  auto finalize_header_if_ready() -> void {
    // 归并分段 header 回调内容。
    if (_current_header_field.empty()) {
      return;
    }
    _current_headers[lower_ascii(_current_header_field)] =
        trim_copy(_current_header_value);
    _current_header_field.clear();
    _current_header_value.clear();
  }

  static auto method_from_llhttp(llhttp_t *parser) -> HttpMethod {
    auto method_name = llhttp_method_name(
        static_cast<llhttp_method_t>(llhttp_get_method(parser)));
    if (method_name == nullptr) {
      return HttpMethod::GET;
    }
    return string_to_http_method(method_name);
  }

  static auto on_message_begin(llhttp_t *parser) -> int {
    auto *self = static_cast<Http1ServerSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    // 开始新请求：重置聚合状态。
    self->_current_method = HttpMethod::GET;
    self->_current_url.clear();
    self->_current_headers.clear();
    self->_current_body.clear();
    self->_current_header_field.clear();
    self->_current_header_value.clear();
    self->_reading_header_value = false;
    return HPE_OK;
  }

  static auto on_url(llhttp_t *parser, const char *at, size_t length) -> int {
    auto *self = static_cast<Http1ServerSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    self->_current_url.append(at, length);
    return HPE_OK;
  }

  static auto on_header_field(llhttp_t *parser, const char *at,
                              size_t length) -> int {
    auto *self = static_cast<Http1ServerSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    if (self->_reading_header_value) {
      self->finalize_header_if_ready();
      self->_reading_header_value = false;
    }

    self->_current_header_field.append(at, length);
    return HPE_OK;
  }

  static auto on_header_value(llhttp_t *parser, const char *at,
                              size_t length) -> int {
    auto *self = static_cast<Http1ServerSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    self->_reading_header_value = true;
    self->_current_header_value.append(at, length);
    return HPE_OK;
  }

  static auto on_headers_complete(llhttp_t *parser) -> int {
    auto *self = static_cast<Http1ServerSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    self->finalize_header_if_ready();
    // 从 parser 中提取 method。
    self->_current_method = method_from_llhttp(parser);
    return HPE_OK;
  }

  static auto on_body(llhttp_t *parser, const char *at, size_t length) -> int {
    auto *self = static_cast<Http1ServerSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    auto start = reinterpret_cast<const uint8_t *>(at);
    self->_current_body.insert(self->_current_body.end(), start, start + length);
    return HPE_OK;
  }

  static auto on_message_complete(llhttp_t *parser) -> int {
    auto *self = static_cast<Http1ServerSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    // 组装业务层请求对象并压入待处理队列。
    HttpRequest request(self->_current_method, std::move(self->_current_url),
                        std::move(self->_current_headers),
                        std::move(self->_current_body));
    self->_pending_requests.push(
        PendingRequest{std::move(request), llhttp_should_keep_alive(parser) == 1});
    return HPE_OK;
  }

private:
  faio::net::detail::TcpStream _stream;
  llhttp_t _parser{};
  llhttp_settings_t _settings{};

  HttpMethod _current_method = HttpMethod::GET;
  std::string _current_url;
  HttpHeaders _current_headers;
  std::vector<uint8_t> _current_body;
  std::string _current_header_field;
  std::string _current_header_value;
  bool _reading_header_value = false;
  bool _keep_alive = true;

  std::queue<PendingRequest> _pending_requests;
};

using ServerSessionAdapterV1 = Http1ServerSession;

} // namespace faio::detail::http

#endif // FAIO_DETAIL_HTTP_SERVER_SESSION_V1_HPP