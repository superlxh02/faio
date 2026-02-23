#ifndef FAIO_DETAIL_HTTP_SERVER_SESSION_V1_HPP
#define FAIO_DETAIL_HTTP_SERVER_SESSION_V1_HPP

#include "faio/detail/common/error.hpp"
#include "faio/detail/http/types.hpp"
#include "faio/detail/net.hpp"
#include "fastlog/fastlog.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <llhttp.h>
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

    _header_block_buf.reserve(512);
    _current_url.reserve(256);
    _current_header_field.reserve(64);
    _current_header_value.reserve(256);
    _current_body.reserve(1024);
    _current_headers.reserve(16);
  }

  Http1ServerSession(const Http1ServerSession &) = delete;
  Http1ServerSession &operator=(const Http1ServerSession &) = delete;
  Http1ServerSession(Http1ServerSession &&) = delete;
  Http1ServerSession &operator=(Http1ServerSession &&) = delete;

  auto run(const Handler &handler, std::span<const uint8_t> initial_data = {})
      -> task<void> {
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

    std::array<char, 4096> buf;
    while (true) {
      // 1) 读取网络数据。
      auto read_res = co_await _stream.read(std::span(buf.data(), buf.size()));
      if (!read_res) {
        fastlog::console.debug("http/1.1 read finished: {}",
                               read_res.error().message());
        break;
      }

      auto len = read_res.value();
      // 2)处理接受到数据是0的情况
      if (len == 0) {
        // 1) EOF：通知 llhttp 做收尾解析。
        auto finish_err = llhttp_finish(&_parser);
        if (finish_err != HPE_OK) {
          fastlog::console.warn("http/1.1 finish parse failed: {} reason={}",
                                llhttp_errno_name(finish_err),
                                llhttp_get_error_reason(&_parser)
                                    ? llhttp_get_error_reason(&_parser)
                                    : "");
        }

        // 2) EOF 前可能仍有已解析请求，继续 flush。
        auto flush_res = co_await consume_pending_requests(handler);
        if (!flush_res) {
          fastlog::console.error("http/1.1 flush pending failed on eof: {}",
                                 flush_res.error().message());
        }
        break;
      }

      // 3) 把本次字节流喂给 llhttp。
      auto proc_res = process_received_data(
          std::span(reinterpret_cast<const uint8_t *>(buf.data()), len));
      if (!proc_res) {
        fastlog::console.warn("http/1.1 parse error: {}",
                              proc_res.error().message());
        break;
      }

      // 4) 处理本轮解析产出的所有完整请求。
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
    fastlog::console.debug("http/1.1 server session loop end, fd={}",
                           _stream.fd());
  }

private:
  struct PendingRequest {
    HttpRequest request;
    bool keep_alive = true;
  };

  // 把字符串转换为小写
  static auto lower_ascii(std::string value) -> std::string {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
  }

  // 去掉字符串两端的空格
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

  // 把字符串转换为小写，原地修改
  static auto lower_ascii_inplace(std::string &value) -> void {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  }

  static auto trim_inplace(std::string &value) -> void {
    auto not_space = [](unsigned char ch) {
      return ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n';
    };

    auto first = std::find_if(value.begin(), value.end(), not_space);
    auto last = std::find_if(value.rbegin(), value.rend(), not_space).base();
    if (first >= last) {
      value.clear();
      return;
    }
    if (first != value.begin() || last != value.end()) {
      value = std::string(first, last);
    }
  }

  // 根据状态码返回对应的错误原因短语
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

  // 组装状态行
  static auto append_status_line(std::string &out, int status) -> void {
    switch (status) {
    case 200:
      out.append("HTTP/1.1 200 OK\r\n");
      return;
    case 201:
      out.append("HTTP/1.1 201 Created\r\n");
      return;
    case 204:
      out.append("HTTP/1.1 204 No Content\r\n");
      return;
    case 400:
      out.append("HTTP/1.1 400 Bad Request\r\n");
      return;
    case 404:
      out.append("HTTP/1.1 404 Not Found\r\n");
      return;
    case 405:
      out.append("HTTP/1.1 405 Method Not Allowed\r\n");
      return;
    case 500:
      out.append("HTTP/1.1 500 Internal Server Error\r\n");
      return;
    default:
      break;
    }

    out.append("HTTP/1.1 ");
    out.append(std::to_string(status));
    auto reason = status_reason_phrase(status);
    if (!reason.empty()) {
      out.push_back(' ');
      out.append(reason.data(), reason.size());
    }
    out.append("\r\n");
  }

  // 把网络层读到的字节流交给 llhttp 增量解析。
  auto process_received_data(std::span<const uint8_t> data) -> expected<void> {
    // 把网络层读到的字节流交给 llhttp 增量解析。
    auto err = llhttp_execute(
        &_parser, reinterpret_cast<const char *>(data.data()), data.size());
    if (err != HPE_OK) {
      return std::unexpected(make_error(EPROTO));
    }
    return expected<void>();
  }

  // 处理待处理请求队列中的请求。
  auto consume_pending_requests(const Handler &handler)
      -> task<expected<void>> {
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

  // 组装响应头并保证 content-length / connection 语义完整。`
  auto write_response(const HttpResponse &resp, bool keep_alive)
      -> task<expected<void>> {
    // 组装响应头并保证 content-length / connection 语义完整。
    // 清空头块缓冲区
    _header_block_buf.clear();
    // 预留头块缓冲区大小
    _header_block_buf.reserve(256 + resp.body().size());
    // 组装状态行，把状态码转换为状态行
    append_status_line(_header_block_buf, resp.status());

    // 标记关键头是否已显式设置。
    auto has_content_length = false;
    auto has_connection = false;
    // 比较两个字符串是否相等，忽略大小写
    auto iequals_ascii = [](std::string_view lhs, std::string_view rhs) {
      if (lhs.size() != rhs.size()) {
        return false;
      }
      for (size_t i = 0; i < lhs.size(); ++i) {
        auto a = static_cast<unsigned char>(lhs[i]);
        auto b = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(a) != std::tolower(b)) {
          return false;
        }
      }
      return true;
    };

    // 遍历响应头，如果头字段是 content-length 或 connection，则标记为已设置
    // ，否则添加到头块缓冲区，如果 content-length 未设置，则自动补上
    // 如果 connection 未设置，则按 keep_alive 参数补上
    for (const auto &[name, value] : resp.headers()) {
      if (iequals_ascii(name, "content-length")) {
        has_content_length = true;
      } else if (iequals_ascii(name, "connection")) {
        has_connection = true;
      }
      _header_block_buf.append(name);
      _header_block_buf.append(": ");
      _header_block_buf.append(value);
      _header_block_buf.append("\r\n");
    }

    if (!has_content_length) {
      // 未设置时自动补 content-length。
      _header_block_buf.append("content-length: ");
      _header_block_buf.append(std::to_string(resp.body().size()));
      _header_block_buf.append("\r\n");
    }

    if (!has_connection) {
      // 未设置时按 keep_alive 参数补 connection。
      _header_block_buf.append("connection: ");
      _header_block_buf.append(keep_alive ? "keep-alive" : "close");
      _header_block_buf.append("\r\n");
    }

    _header_block_buf.append("\r\n");

    // 如果响应体为空，则直接写入头块缓冲区
    if (resp.body().empty()) {
      auto write_res = co_await _stream.write_all(std::span<const char>(
          _header_block_buf.data(), _header_block_buf.size()));
      if (!write_res) {
        co_return std::unexpected(write_res.error());
      }
      co_return expected<void>();
    }

    // 当前头块缓冲区指针位置
    const char *header_pos = _header_block_buf.data();
    // 当前头块缓冲区剩余大小
    std::size_t header_to_write = _header_block_buf.size();

    // 当前响应体指针位置
    const char *body_pos = reinterpret_cast<const char *>(resp.body().data());
    std::size_t body_to_write = resp.body().size();

    // 循环写入头块缓冲区和响应体
    while (header_to_write > 0 || body_to_write > 0) {
      // 头块缓冲区span
      std::span<const char> header_span(
          header_pos, header_to_write > 0 ? header_to_write : 0);
      // 响应体span
      std::span<const char> body_span(body_pos,
                                      body_to_write > 0 ? body_to_write : 0);

      // 写入头块缓冲区和响应体
      auto write_res = co_await _stream.write_v(header_span, body_span);
      // 如果写入失败，则返回错误
      if (!write_res) {
        co_return std::unexpected(write_res.error());
      }
      // 写入的字节数
      auto written = write_res.value();
      // 如果写入的字节数为0，则返回错误
      if (written == 0) {
        co_return std::unexpected(make_error(Error::WriteZero));
      }

      // 以下部分都是在通过更新头部缓冲区和body的指针来控制合并写入的优先级

      // 如果头部缓冲区还有剩余，先满足头部缓冲区数据的发送
      if (header_to_write > 0) {
        // 如果socket发送成功的字节数大于等于现在头部缓冲区需要写入的所有数据
        // 说明头部缓冲区一次性发送成功，
        if (written >= header_to_write) {
          // 写入的字节数减去头部缓冲区需要写入的所有数据
          written -= header_to_write;
          // 头部缓冲区指针位置加上头部缓冲区需要写入的所有数据
          header_pos += header_to_write;
          // 头部缓冲区需要写入大小为0
          header_to_write = 0;
        } else {
          // 否则，如果头部没一次性发送完成
          // 头部缓冲区指针位置加上socket发送成功的字节数
          header_pos += written;
          // 头部缓冲区需要写入大小减去socket发送成功的字节数
          header_to_write -= written;
          // 写入的字节数为0
          written = 0;
        }
      }

      // 如果响应体还有剩余并且socket发送成功的字节数大于0，剩下的再给body
      if (written > 0 && body_to_write > 0) {
        // 如果socket发送成功的字节数大于等于现在响应体需要写入的所有数据
        // 说明响应体一次性发送成功
        if (written >= body_to_write) {
          // 响应体指针位置加上响应体需要写入的所有数据
          body_pos += body_to_write;
          // 响应体需要写入大小为0
          body_to_write = 0;
        } else {
          // 否则，如果响应体没一次性发送完成
          // 响应体指针位置加上socket发送成功的字节数
          body_pos += written;
          // 响应体需要写入大小减去socket发送成功的字节数
          body_to_write -= written;
        }
      }
    }

    co_return expected<void>();
  }

  // 如果当前header字段不为空，则归并分段header回调内容
  auto finalize_header_if_ready() -> void {
    // 归并分段 header 回调内容。
    if (_current_header_field.empty()) {
      return;
    }
    lower_ascii_inplace(_current_header_field);
    trim_inplace(_current_header_value);
    _current_headers[std::move(_current_header_field)] =
        std::move(_current_header_value);
    _current_header_field.clear();
    _current_header_value.clear();
  }

  // 从 llhttp 解析器中提取 method。
  static auto method_from_llhttp(llhttp_t *parser) -> HttpMethod {
    auto method_name = llhttp_method_name(
        static_cast<llhttp_method_t>(llhttp_get_method(parser)));
    if (method_name == nullptr) {
      return HttpMethod::GET;
    }
    return string_to_http_method(method_name);
  }

private:
  // 消息开始回调:重置聚合状态
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

  // URL回调:解析出URL
  static auto on_url(llhttp_t *parser, const char *at, size_t length) -> int {
    auto *self = static_cast<Http1ServerSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    self->_current_url.append(at, length);
    return HPE_OK;
  }

  // header字段回调:解析出header字段
  static auto on_header_field(llhttp_t *parser, const char *at, size_t length)
      -> int {
    auto *self = static_cast<Http1ServerSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    // 如果正在读取header值，则归并分段header回调内容
    if (self->_reading_header_value) {
      // 归并分段header回调内容
      self->finalize_header_if_ready();
      // 设置正在读取header值为false
      self->_reading_header_value = false;
    }

    // 追加header字段
    self->_current_header_field.append(at, length);
    // 返回成功
    return HPE_OK;
  }

  // header值回调:解析出header的值
  static auto on_header_value(llhttp_t *parser, const char *at, size_t length)
      -> int {
    auto *self = static_cast<Http1ServerSession *>(parser->data);
    if (self == nullptr) {
      return HPE_USER;
    }

    // 设置正在读取header值为true
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
    self->_current_body.insert(self->_current_body.end(), start,
                               start + length);
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
    self->_pending_requests.push(PendingRequest{
        std::move(request), llhttp_should_keep_alive(parser) == 1});
    return HPE_OK;
  }

private:
  faio::net::detail::TcpStream _stream; // 网络层流
  llhttp_t _parser{};                   // llhttp 解析器
  llhttp_settings_t _settings{};        // llhttp 设置

  HttpMethod _current_method = HttpMethod::GET; // 当前请求的方法
  std::string _current_url;                     // 当前请求的URL
  HttpHeaders _current_headers;
  std::vector<uint8_t> _current_body; // 当前请求的body
  std::string _current_header_field;  // 当前请求的header字段
  std::string _current_header_value;  // 当前请求的header值
  std::string _header_block_buf;      // 头块缓冲区
  bool _reading_header_value = false; // 是否正在读取header值
  bool _keep_alive = true;            // 是否保持连接

  std::queue<PendingRequest> _pending_requests; // 待处理请求队列
};

using ServerSessionAdapterV1 = Http1ServerSession;

} // namespace faio::detail::http

#endif // FAIO_DETAIL_HTTP_SERVER_SESSION_V1_HPP