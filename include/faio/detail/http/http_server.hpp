#ifndef FAIO_DETAIL_HTTP_HTTP_SERVER_HPP
#define FAIO_DETAIL_HTTP_HTTP_SERVER_HPP

#include "faio/detail/common/error.hpp"
#include "faio/detail/http/v2/server_session_v2.hpp"
#include "faio/detail/http/v1/server_session_v1.hpp"
#include "faio/detail/http/types.hpp"
#include "faio/detail/runtime/context.hpp"
#include "fastlog/fastlog.hpp"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace faio::detail::http {

// handler 类型：进来一个请求，回出去一个响应
using HttpHandler = std::function<task<HttpResponse>(const HttpRequest &)>;
using HttpErrorHandler =
    std::function<task<HttpResponse>(const HttpRequest &, std::string_view)>;

class HttpMiddlewareResult {
public:
  // 继续执行后续中间件/路由。
  static auto next() -> HttpMiddlewareResult {
    return HttpMiddlewareResult();
  }

  // 立即返回响应并短路后续流程。
  static auto respond(HttpResponse response) -> HttpMiddlewareResult {
    HttpMiddlewareResult result;
    result._handled = true;
    result._response = std::move(response);
    return result;
  }

  [[nodiscard]] auto handled() const noexcept -> bool { return _handled; }

  [[nodiscard]] auto response() const noexcept -> const HttpResponse & {
    return _response;
  }

private:
  bool _handled = false;
  HttpResponse _response;
};

using HttpMiddleware =
    std::function<task<HttpMiddlewareResult>(const HttpRequest &)>;

// 轻量路由器：按 method + path 分发。
//
// 分发优先级：
// 1) 中间件短路响应
// 2) 静态路由（method + path）
// 3) 静态路由（any method + path）
// 4) 动态路由（:id / *path）
// 5) fallback
class HttpRouter {
public:
  auto use(HttpMiddleware middleware) -> HttpRouter & {
    _middlewares.push_back(std::move(middleware));
    return *this;
  }

  auto on_error(HttpErrorHandler handler) -> HttpRouter & {
    _error_handler = std::move(handler);
    return *this;
  }

  auto handle(HttpMethod method, std::string path, HttpHandler handler)
      -> HttpRouter & {
    register_route(std::make_optional(method), std::move(path),
                   std::move(handler));
    return *this;
  }

  auto handle(std::string path, HttpHandler handler) -> HttpRouter & {
    register_route(std::nullopt, std::move(path), std::move(handler));
    return *this;
  }

  auto all(std::string path, HttpHandler handler) -> HttpRouter & {
    return handle(std::move(path), std::move(handler));
  }

  auto get(std::string path, HttpHandler handler) -> HttpRouter & {
    return handle(HttpMethod::GET, std::move(path), std::move(handler));
  }

  auto post(std::string path, HttpHandler handler) -> HttpRouter & {
    return handle(HttpMethod::POST, std::move(path), std::move(handler));
  }

  auto put(std::string path, HttpHandler handler) -> HttpRouter & {
    return handle(HttpMethod::PUT, std::move(path), std::move(handler));
  }

  auto del(std::string path, HttpHandler handler) -> HttpRouter & {
    return handle(HttpMethod::DELETE, std::move(path), std::move(handler));
  }

  auto patch(std::string path, HttpHandler handler) -> HttpRouter & {
    return handle(HttpMethod::PATCH, std::move(path), std::move(handler));
  }

  auto fallback(HttpHandler handler) -> HttpRouter & {
    _fallback_handler = std::move(handler);
    return *this;
  }

  auto dispatch(const HttpRequest &req) const -> task<HttpResponse> {
    // 1) 先执行中间件链。
    //    任何一个中间件返回 handled=true 都会直接短路并返回响应。
    for (const auto &middleware : _middlewares) {
      // 每个中间件都通过 safe 包装调用，保证异常不会逃逸到调度层。
      auto middleware_result = co_await invoke_middleware_safe(middleware, req);
      // 中间件决定拦截时，立即返回。
      if (middleware_result.handled()) {
        co_return middleware_result.response();
      }
    }

    // 路由匹配不看 query，只看纯 path。
    auto clean_path = strip_query(req.path());

    // 2) 精确匹配（method + path）优先级最高。
    auto route_it = _routes.find({req.method(), std::string(clean_path)});
    if (route_it != _routes.end()) {
      // 命中后调用业务 handler；内部同样走异常保护。
      co_return co_await invoke_handler_safe(route_it->second, req);
    }

    // 3) 再尝试“任意 method”的精确路径。
    auto any_it = _any_method_routes.find(std::string(clean_path));
    if (any_it != _any_method_routes.end()) {
      co_return co_await invoke_handler_safe(any_it->second, req);
    }

    // 4) 最后再走动态路由（:id、*path）。
    //    动态路由会在命中时把 path 参数注入请求副本。
    auto matched = co_await dispatch_dynamic(req);
    if (matched.matched) {
      co_return std::move(matched.response);
    }

    // 5) 用户自定义兜底。
    if (_fallback_handler) {
      co_return co_await invoke_handler_safe(_fallback_handler, req);
    }

    // 6) 框架默认 404。
    co_return HttpResponseBuilder(404)
        .header("content-type", "text/plain")
        .body("Not Found\n")
        .build();
  }

private:
  struct DynamicDispatchResult {
    bool matched = false;
    HttpResponse response;
  };

  struct DynamicRoute {
    std::optional<HttpMethod> method;
    std::vector<std::string> segments;
    bool has_wildcard = false;
    std::string wildcard_name;
    HttpHandler handler;
  };

  static auto strip_query(std::string_view path) -> std::string_view {
    auto query_pos = path.find('?');
    if (query_pos == std::string_view::npos) {
      return path;
    }
    return path.substr(0, query_pos);
  }

  static auto split_path(std::string_view path) -> std::vector<std::string_view> {
    std::vector<std::string_view> segments;
    // 先去掉 query 部分，避免影响路由分段。
    auto clean = strip_query(path);
    size_t start = 0;
    // 跳过前导 '/'.
    while (start < clean.size() && clean[start] == '/') {
      ++start;
    }

    while (start < clean.size()) {
      // 找到下一个分隔符，截出一段。
      auto end = clean.find('/', start);
      if (end == std::string_view::npos) {
        end = clean.size();
      }
      if (end > start) {
        segments.push_back(clean.substr(start, end - start));
      }
      start = end + 1;
      // 折叠连续 '/'.
      while (start < clean.size() && clean[start] == '/') {
        ++start;
      }
    }
    return segments;
  }

  static auto is_dynamic_path(std::string_view path) -> bool {
    return path.find(':') != std::string::npos || path.find('*') != std::string::npos;
  }

  auto register_route(std::optional<HttpMethod> method, std::string path,
                      HttpHandler handler) -> void {
    // 对输入 path 做规范化（去 query），确保注册和匹配规则一致。
    auto clean_path = std::string(strip_query(path));
    // 静态路由走 map，查找复杂度 O(logN)。
    if (!is_dynamic_path(clean_path)) {
      if (method.has_value()) {
        _routes[{*method, clean_path}] = std::move(handler);
      } else {
        _any_method_routes[clean_path] = std::move(handler);
      }
      return;
    }

    // 动态路由提前编译为“分段模式”，运行时只做线性匹配。
    DynamicRoute route;
    route.method = method;
    route.handler = std::move(handler);

    auto segments = split_path(clean_path);
    route.segments.reserve(segments.size());
    for (size_t i = 0; i < segments.size(); ++i) {
      auto segment = segments[i];
      // *path 表示“后面的所有段都吞掉”
      if (!segment.empty() && segment[0] == '*') {
        route.has_wildcard = true;
        route.wildcard_name =
            segment.size() > 1 ? std::string(segment.substr(1)) : "wildcard";
        break;
      }
      route.segments.emplace_back(segment);
    }

    _dynamic_routes.push_back(std::move(route));
  }

  static auto join_path_segments(const std::vector<std::string_view> &segments,
                                 size_t from) -> std::string {
    std::string out;
    for (size_t i = from; i < segments.size(); ++i) {
      if (!out.empty()) {
        out.push_back('/');
      }
      out.append(segments[i].begin(), segments[i].end());
    }
    return out;
  }

  static auto match_dynamic_route(
      const DynamicRoute &route, const HttpRequest &req,
      std::map<std::string, std::string> &params) -> bool {
    // method 不匹配时快速失败。
    if (route.method.has_value() && *route.method != req.method()) {
      return false;
    }

    // 请求路径按同样规则分段。
    auto req_segments = split_path(req.path());

    // 无通配符必须段数完全一致；有通配符则请求段数 >= 模式段数
    if (!route.has_wildcard && req_segments.size() != route.segments.size()) {
      return false;
    }
    if (route.has_wildcard && req_segments.size() < route.segments.size()) {
      return false;
    }

    for (size_t i = 0; i < route.segments.size(); ++i) {
      auto pattern = std::string_view(route.segments[i]);
      auto actual = req_segments[i];

      // :id 这种动态段，提取参数
      if (!pattern.empty() && pattern[0] == ':') {
        if (pattern.size() <= 1) {
          return false;
        }
        params.emplace(std::string(pattern.substr(1)), std::string(actual));
        continue;
      }

      // 普通静态段要求完全相等。
      if (pattern != actual) {
        return false;
      }
    }

    if (route.has_wildcard) {
      // 把剩余路径拼回去放到通配符参数里
      params.emplace(route.wildcard_name,
                     join_path_segments(req_segments, route.segments.size()));
    }

    return true;
  }

  // 线性扫描动态路由，命中后构造注入 path params 的请求副本。
  auto dispatch_dynamic(const HttpRequest &req) const
      -> task<DynamicDispatchResult> {
    for (const auto &route : _dynamic_routes) {
      std::map<std::string, std::string> params;
      if (!match_dynamic_route(route, req, params)) {
        continue;
      }

      // 这里拷贝一份请求，再注入路由参数，避免污染原请求
      auto matched_req = req;
      matched_req.set_route_params(std::move(params));
      co_return DynamicDispatchResult{
          true, co_await invoke_handler_safe(route.handler, matched_req)};
    }

    co_return DynamicDispatchResult{false, HttpResponseBuilder(404).build()};
  }

  // 捕获业务 handler 异常，统一走 error handler 或默认 500。
  auto invoke_handler_safe(const HttpHandler &handler, const HttpRequest &req) const
      -> task<HttpResponse> {
    std::string err_message;
    try {
      co_return co_await handler(req);
    } catch (const std::exception &e) {
      err_message = e.what();
    } catch (...) {
      err_message = "unknown exception";
    }

    if (_error_handler) {
      // 用户配了错误处理器就交给用户
      co_return co_await _error_handler(req, err_message);
    }

    // 默认兜底 500
    co_return HttpResponseBuilder(500)
        .header("content-type", "text/plain")
        .body("Internal Server Error\n")
        .build();
  }

  auto invoke_middleware_safe(const HttpMiddleware &middleware,
                              const HttpRequest &req) const
      -> task<HttpMiddlewareResult> {
    // 中间件异常会被收敛到 error handler / 默认 500。
    std::string err_message;
    try {
      co_return co_await middleware(req);
    } catch (const std::exception &e) {
      err_message = e.what();
    } catch (...) {
      err_message = "unknown exception";
    }

    if (_error_handler) {
      co_return HttpMiddlewareResult::respond(
          co_await _error_handler(req, err_message));
    }

    // 中间件抛异常且没配 on_error，就直接返回 500 响应
    co_return HttpMiddlewareResult::respond(
        HttpResponseBuilder(500)
            .header("content-type", "text/plain")
            .body("Internal Server Error\n")
            .build());
  }

  std::map<std::pair<HttpMethod, std::string>, HttpHandler> _routes;
  std::map<std::string, HttpHandler> _any_method_routes;
  std::vector<DynamicRoute> _dynamic_routes;
  std::vector<HttpMiddleware> _middlewares;
  HttpHandler _fallback_handler;
  HttpErrorHandler _error_handler;
};

// 对外的 HTTP 服务端接口。
//
// 能力：
// - 监听 TCP 连接。
// - 首包识别 HTTP/2 preface，自动分流到 HTTP/1.1 或 HTTP/2 会话。
// - 每条连接由独立协程处理。
class HttpServer {
public:
  HttpServer() = delete;

  explicit HttpServer(net::detail::TcpListener listener)
      : _listener(std::move(listener)) {}

  ~HttpServer() = default;

  HttpServer(HttpServer &&) = default;
  HttpServer &operator=(HttpServer &&) = default;

  // 绑定到 host + port。
  static auto bind(const std::string &host, uint16_t port)
      -> expected<HttpServer> {
    auto addr_res = net::detail::SocketAddr::parse(host, port);
    if (!addr_res) {
      return std::unexpected(addr_res.error());
    }
    return bind(addr_res.value());
  }

  // 绑定到地址对象。
  static auto bind(const net::detail::SocketAddr &addr)
      -> expected<HttpServer> {
    auto listener_res = net::detail::TcpListener::bind(addr);
    if (!listener_res) {
      return std::unexpected(listener_res.error());
    }

    return HttpServer(std::move(listener_res.value()));
  }

  // 使用业务 handler 启动服务主循环。
  auto run(const HttpHandler &handler) -> task<void> {
    while (true) {
      // 等待新连接。
      auto accept_res = co_await _listener.accept();
      if (!accept_res) {
        // 监听出错了，就退出循环
        break;
      }

      auto [tcp_stream, peer_addr] = std::move(accept_res.value());
      fastlog::console.debug("http server accepted connection from {}",
                             peer_addr.to_string());

      // 每条连接起一个协程去处理，避免阻塞 accept 循环。
      runtime::detail::runtime_context::spawn(
          handle_connection(std::move(tcp_stream), handler));
    }
  }

      // 使用 router 启动服务（内部转成 handler）。
  auto run(const HttpRouter &router) -> task<void> {
    co_await run([&router](const HttpRequest &req) {
      return router.dispatch(req);
    });
  }

private:
  enum class WireProtocol {
    Http1,
    Http2,
  };

  // 判断当前缓存是否仍可能是 HTTP/2 preface 的前缀。
  static auto is_h2_preface_prefix(std::span<const uint8_t> data) -> bool {
    static constexpr std::string_view kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    auto n = std::min(data.size(), kPreface.size());
    return std::equal(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n),
                      kPreface.begin());
  }

  // 判断是否已完整收到 HTTP/2 preface。
  static auto is_h2_preface_complete(std::span<const uint8_t> data) -> bool {
    static constexpr std::string_view kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    if (data.size() < kPreface.size()) {
      return false;
    }
    return std::equal(kPreface.begin(), kPreface.end(), data.begin());
  }

  // 探测连接协议并返回：协议类型 + 已读取首包数据。
  static auto detect_protocol(net::detail::TcpStream &stream)
      -> task<expected<std::pair<WireProtocol, std::vector<uint8_t>>>> {
    static constexpr size_t kReadChunk = 128;
    static constexpr std::string_view kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

    std::vector<uint8_t> initial_data;
    initial_data.reserve(kReadChunk);

    // 循环读取，直到：
    // 1) 明确不是 h2 preface；或
    // 2) 已完整拿到 h2 preface；或
    // 3) 对端 EOF。
    while (initial_data.size() < kPreface.size()) {
      std::array<char, kReadChunk> buf{};
      auto read_res = co_await stream.read(std::span(buf.data(), buf.size()));
      if (!read_res) {
        co_return std::unexpected(read_res.error());
      }

      auto len = read_res.value();
      if (len == 0) {
        break;
      }

      auto begin = reinterpret_cast<const uint8_t *>(buf.data());
      // 追加本次读取数据，参与协议探测。
      initial_data.insert(initial_data.end(), begin, begin + len);

      // 只要不再满足 preface 前缀，就可立即判定为 HTTP/1.1。
      if (!is_h2_preface_prefix(initial_data)) {
        co_return std::pair{WireProtocol::Http1, std::move(initial_data)};
      }
      // 收满 preface 则判定为 HTTP/2。
      if (is_h2_preface_complete(initial_data)) {
        co_return std::pair{WireProtocol::Http2, std::move(initial_data)};
      }
    }

    if (is_h2_preface_complete(initial_data)) {
      co_return std::pair{WireProtocol::Http2, std::move(initial_data)};
    }
    co_return std::pair{WireProtocol::Http1, std::move(initial_data)};
  }

  // 处理单条连接：探测协议 -> 创建会话 -> 初始化 -> 运行。
  static auto handle_connection(net::detail::TcpStream stream,
                                const HttpHandler &handler) -> task<void> {
    // 第一步：探测线协议。
    auto detect_res = co_await detect_protocol(stream);
    if (!detect_res) {
      fastlog::console.error("http server detect protocol failed: {}",
                             detect_res.error().message());
      co_await stream.close();
      co_return;
    }

    auto [protocol, initial_data] = std::move(detect_res.value());
    // 没有首包数据时直接关闭，避免把空连接交给会话层。
    if (initial_data.empty()) {
      co_await stream.close();
      co_return;
    }

    // 第二步：按探测结果分发到对应会话实现。
    if (protocol == WireProtocol::Http2) {
        auto session = std::make_shared<Http2ServerSession>(std::move(stream));

      auto init_res = co_await session->initialize();
      if (!init_res) {
        fastlog::console.error("http/2 server session init failed: {}",
                               init_res.error().message());
        session->close();
        co_return;
      }
      fastlog::console.debug("http/2 server session initialized");

      // initial_data 里已经包含 preface，直接作为首包喂给会话。
      co_await session->run(handler, std::span<const uint8_t>(
                                     initial_data.data(), initial_data.size()));
      co_return;
    }

    auto session = std::make_shared<Http1ServerSession>(std::move(stream));
    fastlog::console.debug("http/1.1 server session initialized");

    co_await session->run(handler,
                          std::span<const uint8_t>(initial_data.data(),
                                                   initial_data.size()));

    co_return;
  }

  auto close() -> void { _listener.close(); }

private:
  net::detail::TcpListener _listener;
};

} // namespace faio::detail::http

#endif // FAIO_DETAIL_HTTP_HTTP_SERVER_HPP
