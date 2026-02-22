#include "faio/faio.hpp"
#include "faio/http.hpp"
#include "fastlog/fastlog.hpp"

#include <cstdlib>
#include <string>

namespace {

struct HttpStressServerConfig {
  std::string host = "0.0.0.0";
  uint16_t port = 9998;
};

auto run_server(const HttpStressServerConfig &config) -> faio::task<int> {
  auto server_res = faio::http::HttpServer::bind(config.host, config.port);
  if (!server_res) {
    fastlog::console.error("http bind failed: {}", server_res.error().message());
    co_return 1;
  }

  auto server = std::move(server_res.value());

  faio::http::HttpRouter router;
  router.get("/health", [&](const faio::http::HttpRequest &)
                 -> faio::task<faio::http::HttpResponse> {
    co_return faio::http::HttpResponseBuilder(200)
        .header("content-type", "text/plain; charset=utf-8")
        .body("ok")
        .build();
  });

  router.get("/index", [&](const faio::http::HttpRequest &)
                -> faio::task<faio::http::HttpResponse> {
    co_return faio::http::HttpResponseBuilder(200)
        .header("content-type", "text/plain; charset=utf-8")
        .body("hello from http_stress server")
        .build();
  });

  router.fallback([&](const faio::http::HttpRequest &)
                      -> faio::task<faio::http::HttpResponse> {
    co_return faio::http::HttpResponseBuilder(404)
        .header("content-type", "text/plain; charset=utf-8")
        .body("not found")
        .build();
  });

  fastlog::console.info("http stress server listening on http://{}:{}", config.host,
                        config.port);
  fastlog::console.info("ready endpoints: GET /health, GET /index");
  fastlog::console.info("use external tools (wrk/hey/ab/vegeta) for load generation");

  co_await server.run(router);
  co_return 0;
}

} // namespace

int main(int argc, char **argv) {
  fastlog::set_consolelog_level(fastlog::LogLevel::Info);

  HttpStressServerConfig config;
  if (argc > 1) {
    config.host = argv[1];
  }
  if (argc > 2) {
    config.port = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10));
  }


  faio::runtime_context ctx;
  return faio::block_on(ctx, run_server(config));
}
