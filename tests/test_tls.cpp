/**
 * @file test_tls.cpp
 * @brief TLS 名称、证书配置与内存握手的单元测试。
 */
#include <faio/tls/handshake.hpp>
#include <faio/tls/stream.hpp>
#include <faio/detail/tls/detail/bio_engine.hpp>
#include <gtest/gtest.h>
#include <array>
#include <fstream>
#include <iterator>
#include <string>
#ifdef FAIO_BAZEL_RUNFILES
#include "bazel_runfiles.hpp"
#endif

namespace {
using namespace faio::tls;

/** @brief 读取测试证书或私钥。 */
std::string fixture(const char* name) {
#ifdef FAIO_BAZEL_RUNFILES
  std::ifstream file(faio_test::bazel_runfile(std::string("tests/fixtures/") + name), std::ios::binary);
#else
  std::ifstream file(std::string(FAIO_TLS_FIXTURE_DIR) + "/" + name, std::ios::binary);
#endif
  return {std::istreambuf_iterator<char>(file), {}};
}

/** @brief 将待发送的密文交给另一个内存 TLS 引擎。 */
void transfer(detail::BioEngine& source, detail::BioEngine& destination) {
  std::array<char, 16384> bytes{};
  while (source.output_pending()) {
    const auto read = source.take_ciphertext(bytes);
    ASSERT_TRUE(read);
    ASSERT_GT(*read, 0u);
    const auto written = destination.feed_ciphertext(std::span<const char>{bytes}.first(*read));
    ASSERT_TRUE(written);
    ASSERT_EQ(*written, *read);
  }
}
}  // namespace

/** @brief 规范化服务名称，并拒绝无效名称。 */
TEST(Tls, ServerNames) {
  const auto name = ServerName::parse("LOCALHOST.");
  ASSERT_TRUE(name);
  EXPECT_EQ(name->value(), "localhost");
  EXPECT_FALSE(name->is_ip());
  EXPECT_TRUE(ServerName::parse("127.0.0.1")->is_ip());
  EXPECT_FALSE(ServerName::parse(""));
  EXPECT_FALSE(ServerName::parse("*.example.com"));
}

/** @brief 证书和配置构建器检查输入，并保持各配置值相互独立。 */
TEST(Tls, Configuration) {
  EXPECT_FALSE(TrustStore::from_pem("not PEM"));
  EXPECT_FALSE(Identity::from_pem("bad", "bad"));
  const auto roots = TrustStore::from_pem(fixture("ca.pem"));
  ASSERT_TRUE(roots);
  const auto base = ClientConfig::builder(*roots).build();
  ASSERT_TRUE(base);
  const auto derived = base->to_builder().with_alpn({"http/1.1"}).build();
  ASSERT_TRUE(derived);
  EXPECT_TRUE(base->alpn_protocols().empty());
  EXPECT_EQ(derived->alpn_protocols().front(), "http/1.1");
  EXPECT_FALSE(ClientConfig::builder(*roots).with_alpn({"x", "x"}).build());
}

/** @brief 客户端与服务端通过内存握手协商协议并传输明文。 */
TEST(Tls, HandshakeAndData) {
  const auto roots = TrustStore::from_pem(fixture("ca.pem"));
  const auto identity = Identity::from_pem(fixture("server.pem"), fixture("server.key"));
  ASSERT_TRUE(roots);
  ASSERT_TRUE(identity);
  const auto client_config = ClientConfig::builder(*roots).with_alpn({"http/1.1"}).build();
  const auto server_config = ServerConfig::builder(*identity).with_alpn({"http/1.1"}).build();
  ASSERT_TRUE(client_config);
  ASSERT_TRUE(server_config);
  auto client = detail::BioEngine::client(*client_config, *ServerName::parse("localhost"));
  auto server = detail::BioEngine::server(*server_config);
  ASSERT_TRUE(client);
  ASSERT_TRUE(server);
  bool client_ready{}, server_ready{};
  for (int round = 0; round != 32 && (!client_ready || !server_ready); ++round) {
    if (!client_ready) {
      const auto result = client->handshake_step();
      ASSERT_TRUE(result);
      client_ready = result->status == detail::StepStatus::complete;
    }
    transfer(*client, *server);
    if (!server_ready) {
      const auto result = server->handshake_step();
      ASSERT_TRUE(result);
      server_ready = result->status == detail::StepStatus::complete;
    }
    transfer(*server, *client);
  }
  ASSERT_TRUE(client_ready && server_ready);
  EXPECT_EQ(client->connection_info().alpn, "http/1.1");
  const std::string payload{"hello TLS"};
  const auto written = client->write_step(payload);
  ASSERT_TRUE(written);
  transfer(*client, *server);
  std::array<char, 32> bytes{};
  const auto read = server->read_step(bytes);
  ASSERT_TRUE(read);
  EXPECT_EQ(std::string(bytes.data(), read->plaintext_bytes), payload);
}
