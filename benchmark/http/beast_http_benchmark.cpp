#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace net = boost::asio;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;
using net::awaitable;
using net::co_spawn;
using net::detached;
using net::use_awaitable;

namespace {

auto handle_session(tcp::socket socket) -> awaitable<void> {
	boost::beast::tcp_stream stream(std::move(socket));
	boost::beast::flat_buffer buffer;
	try {
		while (true) {
			http::request<http::string_body> req;
			co_await http::async_read(stream, buffer, req, use_awaitable);

			http::response<http::string_body> resp;
			resp.version(req.version());
			resp.keep_alive(req.keep_alive());

			if (req.method() != http::verb::get) {
				resp.result(http::status::method_not_allowed);
				resp.set(http::field::content_type, "text/plain; charset=utf-8");
				resp.body() = "method not allowed";
			} else if (req.target() == "/health") {
				resp.result(http::status::ok);
				resp.set(http::field::content_type, "text/plain; charset=utf-8");
				resp.body() = "ok";
			} else if (req.target() == "/index") {
				resp.result(http::status::ok);
				resp.set(http::field::content_type, "text/plain; charset=utf-8");
				resp.body() = "hello from beast server";
			} else {
				resp.result(http::status::not_found);
				resp.set(http::field::content_type, "text/plain; charset=utf-8");
				resp.body() = "not found";
			}

			resp.prepare_payload();
			co_await http::async_write(stream, resp, use_awaitable);
			if (!resp.keep_alive()) {
				break;
			}
		}
	} catch (...) {
	}

	boost::system::error_code ec;
	stream.socket().shutdown(tcp::socket::shutdown_both, ec);
	stream.socket().close(ec);
	co_return;
}

auto listener(tcp::acceptor acceptor) -> awaitable<void> {
	try {
		while (true) {
			tcp::socket socket = co_await acceptor.async_accept(use_awaitable);
			co_spawn(acceptor.get_executor(), handle_session(std::move(socket)), detached);
		}
	} catch (const std::exception &e) {
		std::cerr << "beast listener error: " << e.what() << std::endl;
	}
	co_return;
}

} // namespace

int main(int argc, char **argv) {
	std::string host = "0.0.0.0";
	uint16_t port = 9997;
	if (argc > 1) {
		host = argv[1];
	}
	if (argc > 2) {
		port = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10));
	}

	net::io_context io_context;
	tcp::endpoint endpoint(net::ip::make_address(host), port);
	tcp::acceptor acceptor(io_context, endpoint);

	std::cout << "beast http benchmark listening on http://" << host << ":" << port << std::endl;
	co_spawn(io_context, listener(std::move(acceptor)), detached);

	const std::size_t workers = std::max<std::size_t>(1, std::thread::hardware_concurrency());
	std::vector<std::thread> pool;
	pool.reserve(workers > 0 ? workers - 1 : 0);
	for (std::size_t i = 1; i < workers; ++i) {
		pool.emplace_back([&io_context]() { io_context.run(); });
	}
	io_context.run();
	for (auto &t : pool) {
		if (t.joinable()) {
			t.join();
		}
	}
	return 0;
}
