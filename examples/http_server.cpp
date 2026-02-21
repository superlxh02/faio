#include "faio/faio.hpp"
#include "faio/http.hpp"
#include "fastlog/fastlog.hpp"
#include <string>

auto http_server() -> faio::task<void> {
  auto server_res = faio::http::HttpServer::bind("127.0.0.1", 9998);
  if (!server_res) {
    fastlog::console.error("http server bind failed: {}",
                           server_res.error().message());
    co_return;
  }
  auto server = std::move(server_res).value();

  faio::http::HttpRouter router;

router.get("/index", [](const faio::http::HttpRequest&)
    -> faio::task<faio::http::HttpResponse> {
    
    co_return faio::http::HttpResponseBuilder(200)
        .header("content-type", "text/html; charset=utf-8")
        .body(R"(<!DOCTYPE html>
<html>
<head>
    <title>Hello World</title>
</head>
<body>
    <h1>Hello World!</h1>
</body>
</html>)")
        .build();
});

  fastlog::console.info("http server listening on http://127.0.0.1:9998");
  co_await server.run(router);
  co_return;
}

int main() {
  fastlog::set_consolelog_level(fastlog::LogLevel::Info);
  faio::runtime_context ctx;
  faio::block_on(ctx, http_server());
  return 0;
}
