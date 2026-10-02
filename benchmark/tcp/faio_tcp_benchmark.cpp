#include "faio/faio.hpp"
#include "faio/log.hpp"

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
			"Content-Length: 16\r\n"
			"Connection: keep-alive\r\n"
			"\r\n"
			"hello benchmark\n";

	while (true) {
		auto read_res = co_await stream.read(buf);
		if (!read_res) {
			faio::log::logger()->debug("tcp read failed: {}", read_res.error().message());
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
				faio::log::logger()->debug("tcp write failed: {}", write_res.error().message());
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
		faio::log::logger()->error("parse address failed: {}", addr_res.error().message());
		co_return 1;
	}

	auto listener_res = faio::net::TcpListener::bind(addr_res.value());
	if (!listener_res) {
		faio::log::logger()->error("bind failed: {}", listener_res.error().message());
		co_return 1;
	}

	auto listener = std::move(listener_res.value());
	faio::log::logger()->info("faio tcp benchmark listening on {}:{}", config.host, config.port);

	while (true) {
		auto accept_res = co_await listener.accept();
		if (!accept_res) {
			faio::log::logger()->error("accept failed: {}", accept_res.error().message());
			co_return 1;
		}
		auto [stream, _peer] = std::move(accept_res.value());
		faio::spawn_detached(handle_connection(std::move(stream)));
	}
}

} // namespace

int main(int argc, char **argv) {
	faio::log::logger()->set_level(spdlog::level::info);

	TcpBenchmarkConfig config;
	if (argc > 1) {
		config.host = argv[1];
	}
	if (argc > 2) {
		config.port = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10));
	}
	// 压测线程数由脚本统一传入，避免各语言默认线程数不同。
  const auto workers=argc>3?std::strtoul(argv[3],nullptr,10):4;
  faio::runtime::configure(faio::ConfigBuilder{}.set_num_workers(workers).build());
  return faio::block_on(run_server(config));
}
