#include "faio/faio.hpp"
#include "faio/http.hpp"
#include "fastlog/fastlog.hpp"
#include <span>
#include <string>
#include <string_view>
#include <vector>

auto span_to_string(std::span<const uint8_t> bytes) -> std::string {
  return std::string(bytes.begin(), bytes.end());
}

auto request_once(faio::http::HttpMethod method, std::string path,
                  faio::http::HttpProtocol protocol,
                  faio::http::HttpHeaders headers = {},
                  std::vector<uint8_t> body = {})
    -> faio::task<faio::expected<faio::http::HttpResponse>> {
  auto stream_res =
      co_await faio::http::HttpStream::connect("127.0.0.1", 9998, protocol);
  if (!stream_res) {
    co_return std::unexpected(stream_res.error());
  }

  auto stream = std::move(stream_res).value();
  auto req = faio::http::HttpRequest::create(method, std::move(path), std::move(headers),
                                       std::move(body));
  auto resp_res = co_await stream.request(req);
  co_await stream.close();
  co_return resp_res;
}

auto expect_ok(std::string_view name,
               const faio::expected<faio::http::HttpResponse> &resp_res,
               int status, std::string_view contains) -> bool {
  if (!resp_res) {
    fastlog::console.error("[FAIL] {} request error: {}", name,
                           resp_res.error().message());
    return false;
  }

  auto body = span_to_string(resp_res->body());
  if (resp_res->status() != status || body.find(contains) == std::string::npos) {
    fastlog::console.error("[FAIL] {} status={} body={}", name, resp_res->status(),
                           body);
    return false;
  }

  fastlog::console.info("[PASS] {} status={} body={}", name, resp_res->status(), body);
  return true;
}

auto http_client() -> faio::task<int> {
  int passed = 0;
  int total = 0;

  ++total;
  auto ping_res =
      co_await request_once(faio::http::HttpMethod::GET, "/api/ping",
                            faio::http::HttpProtocol::Http1,
                            {{"x-request-id", "req-get-1"}});
  if (expect_ok("get-ping", ping_res, 200, "pong")) {
    ++passed;
  }

  ++total;
  auto echo_res = co_await request_once(
      faio::http::HttpMethod::POST, "/api/echo", faio::http::HttpProtocol::Http1,
      {{"x-request-id", "req-post-1"}},
      std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o'});
  if (expect_ok("post-echo", echo_res, 200, "echo: hello")) {
    ++passed;
  }

  ++total;
  auto ping_h2_res =
      co_await request_once(faio::http::HttpMethod::GET, "/api/ping",
                            faio::http::HttpProtocol::Http2,
                            {{"x-request-id", "req-get-h2"}});
  if (expect_ok("get-ping-h2", ping_h2_res, 200, "pong")) {
    ++passed;
  }

  fastlog::console.info("simple http tests: {}/{} passed", passed, total);
  co_return passed == total ? 0 : 1;
}

int main() {
  fastlog::set_consolelog_level(fastlog::LogLevel::Info);
  faio::runtime_context ctx;
  return faio::block_on(ctx, http_client());
}
