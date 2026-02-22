#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using asio::ip::tcp;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::use_awaitable;

namespace {

auto handle_session(tcp::socket socket) -> awaitable<void> {
	std::array<char, 8192> buf{};
	std::string request_buffer;
	request_buffer.reserve(4096);

	static const std::string response =
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain; charset=utf-8\r\n"
			"Content-Length: 20\r\n"
			"Connection: keep-alive\r\n"
			"\r\n"
			"hello from asio tcp\n";

	try {
		while (true) {
			std::size_t n = co_await socket.async_read_some(asio::buffer(buf), use_awaitable);
			if (n == 0) {
				break;
			}

			request_buffer.append(buf.data(), n);
			while (true) {
				auto pos = request_buffer.find("\r\n\r\n");
				if (pos == std::string::npos) {
					break;
				}
				co_await asio::async_write(socket, asio::buffer(response), use_awaitable);
				request_buffer.erase(0, pos + 4);
			}
		}
	} catch (...) {
	}

	std::error_code ec;
	socket.shutdown(tcp::socket::shutdown_both, ec);
	socket.close(ec);
	co_return;
}

auto listener(tcp::acceptor acceptor) -> awaitable<void> {
	try {
		while (true) {
			tcp::socket socket = co_await acceptor.async_accept(use_awaitable);
			co_spawn(acceptor.get_executor(), handle_session(std::move(socket)), detached);
		}
	} catch (const std::exception &e) {
		std::cerr << "asio listener error: " << e.what() << std::endl;
	}
	co_return;
}

} // namespace

int main(int argc, char **argv) {
	std::string host = "0.0.0.0";
	uint16_t port = 18082;
	if (argc > 1) {
		host = argv[1];
	}
	if (argc > 2) {
		port = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10));
	}

	asio::io_context io;
	tcp::endpoint endpoint(asio::ip::make_address(host), port);
	tcp::acceptor acceptor(io, endpoint);
	std::cout << "asio tcp benchmark listening on " << host << ":" << port << std::endl;
	co_spawn(io, listener(std::move(acceptor)), detached);

	const std::size_t workers = std::max<std::size_t>(1, std::thread::hardware_concurrency());
	std::vector<std::thread> pool;
	pool.reserve(workers > 0 ? workers - 1 : 0);
	for (std::size_t i = 1; i < workers; ++i) {
		pool.emplace_back([&io]() { io.run(); });
	}
	io.run();
	for (auto &t : pool) {
		if (t.joinable()) {
			t.join();
		}
	}
	return 0;
}
