#ifndef FAIO_DETAIL_HTTP_TYPES_HPP
#define FAIO_DETAIL_HTTP_TYPES_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace faio::detail::http {

class HttpRouter;

// HTTP 方法枚举
enum class HttpMethod {
  GET,
  POST,
  PUT,
  DELETE,
  HEAD,
  OPTIONS,
  PATCH,
  CONNECT,
  TRACE,
};

// 把 HttpMethod 转成字符串
[[nodiscard]]
inline auto http_method_to_string(HttpMethod method) -> std::string_view {
  switch (method) {
  case HttpMethod::GET:
    return "GET";
  case HttpMethod::POST:
    return "POST";
  case HttpMethod::PUT:
    return "PUT";
  case HttpMethod::DELETE:
    return "DELETE";
  case HttpMethod::HEAD:
    return "HEAD";
  case HttpMethod::OPTIONS:
    return "OPTIONS";
  case HttpMethod::PATCH:
    return "PATCH";
  case HttpMethod::CONNECT:
    return "CONNECT";
  case HttpMethod::TRACE:
    return "TRACE";
  }
  return "GET";
}

// 把字符串转成 HttpMethod
[[nodiscard]]
inline auto string_to_http_method(std::string_view method) -> HttpMethod {
  if (method == "GET")
    return HttpMethod::GET;
  if (method == "POST")
    return HttpMethod::POST;
  if (method == "PUT")
    return HttpMethod::PUT;
  if (method == "DELETE")
    return HttpMethod::DELETE;
  if (method == "HEAD")
    return HttpMethod::HEAD;
  if (method == "OPTIONS")
    return HttpMethod::OPTIONS;
  if (method == "PATCH")
    return HttpMethod::PATCH;
  if (method == "CONNECT")
    return HttpMethod::CONNECT;
  if (method == "TRACE")
    return HttpMethod::TRACE;
  return HttpMethod::GET;
}

// HTTP 头：name -> value
using HttpHeaders = std::map<std::string, std::string>;

// HTTP 请求值对象。
//
// 设计约束：
// 1) 该对象是“按值传递”友好的，请求路径/头/body 均由对象独立持有。
// 2) 路由参数由路由层在匹配成功后回填，业务 handler 只读访问。
// 3) query 参数按需懒解析，避免未使用场景下的额外开销。
class HttpRequest {
public:
  explicit HttpRequest(HttpMethod method = HttpMethod::GET,
                       std::string path = "/", HttpHeaders headers = {},
                       std::vector<uint8_t> body = {})
      : _method(method), _path(std::move(path)), _headers(std::move(headers)),
        _body(std::move(body)) {}

  [[nodiscard]] auto method() const noexcept -> HttpMethod { return _method; }

  [[nodiscard]] auto path() const noexcept -> std::string_view { return _path; }

  [[nodiscard]] auto path_without_query() const noexcept -> std::string_view {
    auto query_pos = _path.find('?');
    if (query_pos == std::string::npos) {
      return _path;
    }
    return std::string_view(_path).substr(0, query_pos);
  }

  [[nodiscard]] auto query_string() const noexcept -> std::string_view {
    auto query_pos = _path.find('?');
    if (query_pos == std::string::npos || query_pos + 1 >= _path.size()) {
      return {};
    }
    return std::string_view(_path).substr(query_pos + 1);
  }

  [[nodiscard]] auto headers() const noexcept -> const HttpHeaders & {
    return _headers;
  }

  [[nodiscard]] auto header(std::string_view name) const
      -> std::optional<std::string_view> {
    auto it = _headers.find(std::string(name));
    if (it == _headers.end()) {
      return std::nullopt;
    }
    return std::string_view(it->second);
  }

  [[nodiscard]] auto body() const noexcept -> std::span<const uint8_t> {
    return _body;
  }

  [[nodiscard]] auto body() noexcept -> std::span<uint8_t> { return _body; }

  [[nodiscard]] auto body_as_string() const -> std::string {
    return std::string(_body.begin(), _body.end());
  }

  [[nodiscard]] auto route_params() const noexcept
      -> const std::map<std::string, std::string> & {
    return _route_params;
  }

  [[nodiscard]] auto path_param(std::string_view name) const
      -> std::optional<std::string_view> {
    auto it = _route_params.find(std::string(name));
    if (it == _route_params.end()) {
      return std::nullopt;
    }
    return std::string_view(it->second);
  }

  [[nodiscard]] auto query_params() const
      -> const std::map<std::string, std::string> & {
    parse_query_params_if_needed();
    return _query_params;
  }

  [[nodiscard]] auto query_param(std::string_view name) const
      -> std::optional<std::string_view> {
    parse_query_params_if_needed();
    auto it = _query_params.find(std::string(name));
    if (it == _query_params.end()) {
      return std::nullopt;
    }
    return std::string_view(it->second);
  }

  static auto create(HttpMethod method, std::string path,
                     HttpHeaders headers = {}, std::vector<uint8_t> body = {})
      -> HttpRequest {
    return HttpRequest(method, std::move(path), std::move(headers),
                       std::move(body));
  }

private:
  static auto from_hex(char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  }

  static auto url_decode(std::string_view input) -> std::string {
    std::string decoded;
    decoded.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
      auto ch = input[i];
      if (ch == '+') {
        decoded.push_back(' ');
        continue;
      }
      if (ch == '%' && i + 2 < input.size()) {
        auto high = from_hex(input[i + 1]);
        auto low = from_hex(input[i + 2]);
        if (high >= 0 && low >= 0) {
          decoded.push_back(static_cast<char>((high << 4) | low));
          i += 2;
          continue;
        }
      }
      decoded.push_back(ch);
    }
    return decoded;
  }

  // 懒解析 query string，结果缓存在 _query_params。
  // 方法是 const，但会更新 mutable 缓存字段。
  auto parse_query_params_if_needed() const -> void {
    if (_query_parsed) {
      return;
    }
    _query_parsed = true;

    auto query = query_string();
    size_t start = 0;
    while (start <= query.size()) {
      auto end = query.find('&', start);
      if (end == std::string_view::npos) {
        end = query.size();
      }

      auto item = query.substr(start, end - start);
      if (!item.empty()) {
        auto eq = item.find('=');
        if (eq == std::string_view::npos) {
          _query_params.emplace(url_decode(item), "");
        } else {
          auto key = url_decode(item.substr(0, eq));
          auto val = url_decode(item.substr(eq + 1));
          _query_params[std::move(key)] = std::move(val);
        }
      }

      if (end == query.size()) {
        break;
      }
      start = end + 1;
    }
  }

  auto set_route_params(std::map<std::string, std::string> params) -> void {
    _route_params = std::move(params);
  }

  HttpMethod _method;
  std::string _path;
  HttpHeaders _headers;
  std::vector<uint8_t> _body;
  std::map<std::string, std::string> _route_params;
  mutable std::map<std::string, std::string> _query_params;
  mutable bool _query_parsed = false;

  friend class HttpRouter;
};

// HTTP 响应值对象。
//
// 约定：
// - status / headers / body 完全由对象管理，可安全跨协程传递。
// - 提供常见状态码工厂函数，便于快速返回简单响应。
class HttpResponse {
public:
  explicit HttpResponse(int status = 200, HttpHeaders headers = {},
                        std::vector<uint8_t> body = {})
      : _status(status), _headers(std::move(headers)), _body(std::move(body)) {}

  [[nodiscard]] auto status() const noexcept -> int { return _status; }

  [[nodiscard]] auto headers() const noexcept -> const HttpHeaders & {
    return _headers;
  }

  [[nodiscard]] auto body() const noexcept -> std::span<const uint8_t> {
    return _body;
  }

  [[nodiscard]] auto body() noexcept -> std::span<uint8_t> { return _body; }

  static auto ok(std::string body = {}) -> HttpResponse {
    HttpResponse resp(200);
    resp._body = std::vector<uint8_t>(body.begin(), body.end());
    return resp;
  }

  static auto created(std::string body = {}) -> HttpResponse {
    HttpResponse resp(201);
    resp._body = std::vector<uint8_t>(body.begin(), body.end());
    return resp;
  }

  static auto bad_request(std::string body = {}) -> HttpResponse {
    HttpResponse resp(400);
    resp._body = std::vector<uint8_t>(body.begin(), body.end());
    return resp;
  }

  static auto not_found(std::string body = {}) -> HttpResponse {
    HttpResponse resp(404);
    resp._body = std::vector<uint8_t>(body.begin(), body.end());
    return resp;
  }

  static auto internal_error(std::string body = {}) -> HttpResponse {
    HttpResponse resp(500);
    resp._body = std::vector<uint8_t>(body.begin(), body.end());
    return resp;
  }

private:
  int _status;
  HttpHeaders _headers;
  std::vector<uint8_t> _body;

  friend class HttpResponseBuilder;
};

// HTTP 响应构造器。
//
// 用于链式拼装响应，避免直接暴露 HttpResponse 内部可写字段。
class HttpResponseBuilder {
public:
  explicit HttpResponseBuilder(int status = 200) : _response(status) {}

  auto status(int status) -> HttpResponseBuilder & {
    _response._status = status;
    return *this;
  }

  auto header(std::string name, std::string value) -> HttpResponseBuilder & {
    _response._headers[std::move(name)] = std::move(value);
    return *this;
  }

  auto headers(HttpHeaders headers) -> HttpResponseBuilder & {
    _response._headers = std::move(headers);
    return *this;
  }

  auto body(std::vector<uint8_t> body) -> HttpResponseBuilder & {
    _response._body = std::move(body);
    return *this;
  }

  auto body(std::string body) -> HttpResponseBuilder & {
    _response._body.clear();
    _response._body.insert(_response._body.end(), body.begin(), body.end());
    return *this;
  }

  auto body(std::span<const uint8_t> body) -> HttpResponseBuilder & {
    _response._body.clear();
    _response._body.insert(_response._body.end(), body.begin(), body.end());
    return *this;
  }

  [[nodiscard]] auto build() -> HttpResponse { return _response; }

private:
  HttpResponse _response;
};

// 便捷函数：直接拿一个 builder 来用
inline auto make_response() -> HttpResponseBuilder {
  return HttpResponseBuilder();
}

} // namespace faio::detail::http

#endif // FAIO_DETAIL_HTTP_TYPES_HPP
