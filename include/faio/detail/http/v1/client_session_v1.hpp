#ifndef FAIO_DETAIL_HTTP_CLIENT_SESSION_V1_HPP
#define FAIO_DETAIL_HTTP_CLIENT_SESSION_V1_HPP

#include "faio/detail/common/error.hpp"
#include "faio/detail/http/types.hpp"
#include "faio/detail/net.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <llhttp.h>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace faio::detail::http {

class Http1ClientSession {
public:
  explicit Http1ClientSession(faio::net::detail::TcpStream stream)
      : _stream(std::move(stream)) {
    // 绑定 llhttp 回调，把解析事件回传到当前会话对象。
    llhttp_settings_init(&_settings);
    _settings.on_message_begin = &Http1ClientSession::on_message_begin;
    _settings.on_header_field = &Http1ClientSession::on_header_field;
    _settings.on_header_value = &Http1ClientSession::on_header_value;
    _settings.on_headers_complete = &Http1ClientSession::on_headers_complete;
    _settings.on_body = &Http1ClientSession::on_body;
    _settings.on_message_complete = &Http1ClientSession::on_message_complete;

    llhttp_init(&_parser, HTTP_RESPONSE, &_settings);
    _parser.data = this;
  }

  Http1ClientSession(const Http1ClientSession &) = delete;
  Http1ClientSession &operator=(const Http1ClientSession &) = delete;
  Http1ClientSession(Http1ClientSession &&) = delete;
  Http1ClientSession &operator=(Http1ClientSession &&) = delete;

  // HTTP/1.1 客户端当前无额外握手，预留接口与 HTTP/2 统一。
  auto initialize() -> task<expected<void>> { co_return expected<void>(); }

  // 发送请求并等待完整响应。
  // 读循环直到 llhttp 回调产出一条完整响应（_pending_responses 非空）。
  auto request(const HttpRequest &req) -> task<expected<HttpResponse>> {
    // 1) 把结构化请求序列化为 HTTP/1.1 报文。
    auto wire_req = serialize_request(req);
    // 2) 先写请求头与 body。
    auto header_write = co_await _stream.write_all(
        std::span<const char>(wire_req.data(), wire_req.size()));
    if (!header_write) {
      co_return std::unexpected(header_write.error());
    }

    // 3) 增量读取并驱动 llhttp 解析，直到拿到完整响应。
    while (_pending_responses.empty()) {
      std::vector<char> buf(8192);
      auto read_res = co_await _stream.read(std::span(buf.data(), buf.size()));
      if (!read_res) {
        co_return std::unexpected(read_res.error());
      }

      auto len = read_res.value();
      if (len == 0) {
        // EOF 时调用 finish，处理可能的尾部状态。
        auto finish_err = llhttp_finish(&_parser);
        if (finish_err != HPE_OK) {
          co_return std::unexpected(make_error(EPROTO));
        }
        break;
      }

      auto err = llhttp_execute(&_parser, buf.data(), len);
      if (err != HPE_OK) {
        co_return std::unexpected(make_error(EPROTO));
      }
    }

    if (_pending_responses.empty()) {
      co_return std::unexpected(
          make_error(static_cast<int>(Error::UnexpectedEOF)));
    }

    // 4) 取出第一条完整响应。
    auto resp = std::move(_pending_responses.front());
    _pending_responses.pop();

    // 一次请求结束后重置 parser，支持同连接复用。
    llhttp_reset(&_parser);
    _parser.data = this;

    co_return resp;
  }

  auto close() noexcept { return _stream.close(); }

private:
  static auto lower_ascii(std::string value) -> std::string {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
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

  static auto format_method(HttpMethod method) -> std::string_view {
    return http_method_to_string(method);
  }

  static auto serialize_request(const HttpRequest &req) -> std::string {
    // 组装标准 HTTP/1.1 请求报文。
    std::string out;
    out.reserve(512 + req.body().size());

    out.append(format_method(req.method()));
    out.push_back(' ');
    auto path = req.path();
    out.append(path.empty() ? "/" : std::string(path));
    out.append(" HTTP/1.1\r\n");

    // 标记是否已显式设置关键头；若没有则补默认值。
    auto has_host = false;
    auto has_content_length = false;
    auto has_connection = false;
    for (const auto &[name, value] : req.headers()) {
      auto lower = lower_ascii(name);
      if (lower == "host") {
        has_host = true;
      } else if (lower == "content-length") {
        has_content_length = true;
      } else if (lower == "connection") {
        has_connection = true;
      }
      out.append(name);
      out.append(": ");
      out.append(value);
      out.append("\r\n");
    }

    if (!has_host) {
      // 没传 host 时给默认值，避免非法报文。
      out.append("host: localhost\r\n");
    }
    if (!has_connection) {
      // 默认 keep-alive，允许连接复用。
      out.append("connection: keep-alive\r\n");
    }
    if (!has_content_length) {
      // 非 chunked 模式下显式写 content-length。
      out.append("content-length: ");
      out.append(std::to_string(req.body().size()));
      out.append("\r\n");
    }

    out.append("\r\n");
    if (!req.body().empty()) {
      out.append(reinterpret_cast<const char *>(req.body().data()),
                 req.body().size());
    }

    return out;
  }

  auto finalize_header_if_ready() -> void {
    // llhttp 可能分段回调 header field/value；
    // 在切换字段或头结束时把当前 pair 刷入 map。
    if (_current_header_field.empty()) {
      return;
    }
    _current_headers[lower_ascii(_current_header_field)] =
        trim_copy(_current_header_value);
    _current_header_field.clear();
    _current_header_value.clear();
  }

  static auto on_message_begin(llhttp_t *parser) -> int {
    auto *self = static_cast<Http1ClientSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    // 重置一次响应聚合缓冲。
    self->_current_status = 200;
    self->_current_headers.clear();
    self->_current_body.clear();
    self->_current_header_field.clear();
    self->_current_header_value.clear();
    self->_reading_header_value = false;
    return HPE_OK;
  }

  static auto on_header_field(llhttp_t *parser, const char *at, size_t length)
      -> int {
    auto *self = static_cast<Http1ClientSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    if (self->_reading_header_value) {
      // field/value 交替到来：遇到新 field 前先提交上一个头。
      self->finalize_header_if_ready();
      self->_reading_header_value = false;
    }

    self->_current_header_field.append(at, length);
    return HPE_OK;
  }

  static auto on_header_value(llhttp_t *parser, const char *at, size_t length)
      -> int {
    auto *self = static_cast<Http1ClientSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    self->_reading_header_value = true;
    self->_current_header_value.append(at, length);
    return HPE_OK;
  }

  static auto on_headers_complete(llhttp_t *parser) -> int {
    auto *self = static_cast<Http1ClientSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    self->finalize_header_if_ready();
    // 从 parser 读取响应状态码。
    self->_current_status = llhttp_get_status_code(parser);
    return HPE_OK;
  }

  static auto on_body(llhttp_t *parser, const char *at, size_t length) -> int {
    auto *self = static_cast<Http1ClientSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    auto start = reinterpret_cast<const uint8_t *>(at);
    self->_current_body.insert(self->_current_body.end(), start,
                               start + length);
    return HPE_OK;
  }

  static auto on_message_complete(llhttp_t *parser) -> int {
    auto *self = static_cast<Http1ClientSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    // 一条完整响应入队，供 request() 侧取走。
    self->_pending_responses.push(
        HttpResponse(self->_current_status, std::move(self->_current_headers),
                     std::move(self->_current_body)));
    return HPE_OK;
  }

private:
  faio::net::detail::TcpStream _stream; // 网络层流
  llhttp_t _parser{};                   // llhttp 解析器
  llhttp_settings_t _settings{};        // llhttp 设置

  int _current_status = 200;          // 当前响应状态码
  HttpHeaders _current_headers;       // 当前响应头
  std::vector<uint8_t> _current_body; // 当前响应体
  std::string _current_header_field;  // 当前响应头字段
  std::string _current_header_value;  // 当前响应头值
  bool _reading_header_value = false; // 是否正在读取响应头值

  std::queue<HttpResponse> _pending_responses; // 待处理的响应队列
};

using ClientSessionAdapterV1 = Http1ClientSession;

} // namespace faio::detail::http

#endif // FAIO_DETAIL_HTTP_CLIENT_SESSION_V1_HPP