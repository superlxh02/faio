#include <gtest/gtest.h>

#include "faio/faio.hpp"
#include "faio/http.hpp"

#include <string>

namespace {

auto body_to_string(const faio::http::HttpResponse& resp) -> std::string {
  auto body = resp.body();
  return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

auto dispatch(faio::runtime_context& ctx,
              const faio::http::HttpRouter& router,
              const faio::http::HttpRequest& req) -> faio::http::HttpResponse {
  return faio::block_on(ctx, router.dispatch(req));
}

}  // namespace

TEST(HttpRouterTest, StaticRouteMatches) {
  faio::runtime_context ctx;
  faio::http::HttpRouter router;

  router.get("/index", [](const faio::http::HttpRequest&) -> faio::task<faio::http::HttpResponse> {
    co_return faio::http::HttpResponseBuilder(200).body("ok").build();
  });

  faio::http::HttpRequest req(faio::http::HttpMethod::GET, "/index");
  auto resp = dispatch(ctx, router, req);

  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(body_to_string(resp), "ok");
}

TEST(HttpRouterTest, DynamicRouteExtractsParam) {
  faio::runtime_context ctx;
  faio::http::HttpRouter router;

  router.get("/users/:id", [](const faio::http::HttpRequest& req) -> faio::task<faio::http::HttpResponse> {
    auto id = req.path_param("id");
    std::string value = id ? std::string(*id) : "none";
    co_return faio::http::HttpResponseBuilder(200).body(value).build();
  });

  faio::http::HttpRequest req(faio::http::HttpMethod::GET, "/users/123");
  auto resp = dispatch(ctx, router, req);

  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(body_to_string(resp), "123");
}

TEST(HttpRouterTest, MiddlewareCanShortCircuit) {
  faio::runtime_context ctx;
  faio::http::HttpRouter router;

  router.use([](const faio::http::HttpRequest&) -> faio::task<faio::http::HttpMiddlewareResult> {
    co_return faio::http::HttpMiddlewareResult::respond(
        faio::http::HttpResponseBuilder(401).body("blocked").build());
  });

  router.get("/index", [](const faio::http::HttpRequest&) -> faio::task<faio::http::HttpResponse> {
    co_return faio::http::HttpResponseBuilder(200).body("ok").build();
  });

  faio::http::HttpRequest req(faio::http::HttpMethod::GET, "/index");
  auto resp = dispatch(ctx, router, req);

  EXPECT_EQ(resp.status(), 401);
  EXPECT_EQ(body_to_string(resp), "blocked");
}

TEST(HttpRouterTest, FallbackReturnsResponse) {
  faio::runtime_context ctx;
  faio::http::HttpRouter router;

  router.fallback([](const faio::http::HttpRequest&) -> faio::task<faio::http::HttpResponse> {
    co_return faio::http::HttpResponseBuilder(404).body("fallback").build();
  });

  faio::http::HttpRequest req(faio::http::HttpMethod::GET, "/missing");
  auto resp = dispatch(ctx, router, req);

  EXPECT_EQ(resp.status(), 404);
  EXPECT_EQ(body_to_string(resp), "fallback");
}
