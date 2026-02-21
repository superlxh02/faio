#ifndef FAIO_HTTP_HPP
#define FAIO_HTTP_HPP

#include "faio/detail/http/types.hpp"
#include "faio/detail/http/http_stream.hpp"
#include "faio/detail/http/http_server.hpp"


namespace faio::http {

using detail::http::HttpMethod;
using detail::http::http_method_to_string;
using detail::http::string_to_http_method;

using detail::http::HttpHeaders;
using detail::http::HttpRequest;
using detail::http::HttpResponse;
using detail::http::HttpResponseBuilder;
using detail::http::make_response;

using detail::http::HttpProtocol;
using detail::http::HttpStream;

using detail::http::HttpHandler;
using detail::http::HttpErrorHandler;
using detail::http::HttpMiddleware;
using detail::http::HttpMiddlewareResult;
using detail::http::HttpRouter;
using detail::http::HttpServer;

} // namespace faio::http

#endif // FAIO_HTTP_HPP
