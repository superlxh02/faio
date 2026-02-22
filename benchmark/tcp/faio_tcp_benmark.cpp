#include "faio/faio.hpp"
#include "fastlog/fastlog.hpp"

#include <array>
#include <cstdlib>
#include <string>

namespace {

struct TcpBenchmarkConfig {
	std::string host = "0.0.0.0";
	uint16_t port = 18081;
};

auto handle_connection(faio::net::TcpStream stream) -> faio::task<void> {
	std::array<char, 8192> buf{};
	std::string request_buffer;
	request_buffer.reserve(4096);

	static const std::string response =
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain; charset=utf-8\r\n"
			"Content-Length: 20\r\n"
			"Connection: keep-alive\r\n"
			"\r\n"
			"hello from faio tcp\n";

	while (true) {
		auto read_res = co_await stream.read(buf);
		if (!read_res) {
			fastlog::console.debug("tcp read failed: {}", read_res.error().message());
			break;
		}

		auto len = read_res.value();
		if (len == 0) {
			break;
		}

		request_buffer.append(buf.data(), len);

		while (true) {
			auto end = request_buffer.find("\r\n\r\n");
			if (end == std::string::npos) {
				break;
			}

			auto write_res = co_await stream.write_all(
					std::span<const char>(response.data(), response.size()));
			if (!write_res) {
				fastlog::console.debug("tcp write failed: {}", write_res.error().message());
				co_return;
			}
			request_buffer.erase(0, end + 4);
		}
	}

	co_return;
}

auto run_server(const TcpBenchmarkConfig &config) -> faio::task<int> {
	auto addr_res = faio::net::address::parse(config.host, config.port);
	if (!addr_res) {
		fastlog::console.error("parse address failed: {}", addr_res.error().message());
		co_return 1;
	}

	auto listener_res = faio::net::TcpListener::bind(addr_res.value());
	if (!listener_res) {
		fastlog::console.error("bind failed: {}", listener_res.error().message());
		co_return 1;
	}

	auto listener = std::move(listener_res.value());
	fastlog::console.info("faio tcp benchmark listening on {}:{}", config.host, config.port);

	while (true) {
		auto accept_res = co_await listener.accept();
		if (!accept_res) {
			fastlog::console.error("accept failed: {}", accept_res.error().message());
			co_return 1;
		}
		auto [stream, _peer] = std::move(accept_res.value());
		faio::spawn(handle_connection(std::move(stream)));
	}
}

} // namespace

int main(int argc, char **argv) {
	fastlog::set_consolelog_level(fastlog::LogLevel::Info);

	TcpBenchmarkConfig config;
	if (argc > 1) {
		config.host = argv[1];
	}
	if (argc > 2) {
		config.port = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10));
	}
	faio::runtime_context ctx;
	return faio::block_on(ctx, run_server(config));
}
