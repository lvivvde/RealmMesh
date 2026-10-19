#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include "realmmesh/network/client/edge_client_connection.hpp"
#include "realmmesh/network/transport/transport_factory.hpp"

namespace realm::network {
namespace {
using namespace std::chrono_literals;
std::string identity(std::string_view file) {
    return (std::filesystem::path(REALMMESH_TEST_SERVICE_TLS_DIRECTORY) / file)
        .string();
}
class MutualTlsTest : public ::testing::Test {
protected:
    void SetUp() override {
        TransportConfig config;
        config.name = "internal";
        config.listen_address = "127.0.0.1";
        config.tls = TransportConfig::TlsServerIdentity{
            .certificate_chain_file = identity("scene.pem"),
            .private_key_file = identity("scene.key"),
            .alpn = "test-internal/1",
            .client_ca_file = identity("ca.pem"),
            .allowed_client_identities = {"realm-dev-01.realm.service"}};
        auto transports = TransportFactory::create_enabled(
            std::vector<TransportConfig>{config});
        server_ = std::move(transports.front());
        port_ = server_->local_endpoint().port;
        worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                for (auto& event : server_->poll_once(2ms)) {
                    if (event.kind == TransportEventKind::SessionOpened)
                        ++opened_;
                    if (event.kind == TransportEventKind::MessageReceived) {
                        ++messages_;
                        static_cast<void>(
                            server_->send(event.session_id, event.payload));
                    }
                }
            }
        });
    }
    void TearDown() override {
        worker_.request_stop();
        worker_.join();
        server_.reset();
    }
    client::TlsClientOptions options() {
        client::TlsClientOptions value;
        value.alpn = "test-internal/1";
        value.ca_file = identity("ca.pem");
        value.server_identity = "scene-dev-01.scene.service";
        value.certificate_chain_file = identity("realm.pem");
        value.private_key_file = identity("realm.key");
        return value;
    }
    bool roundtrip(const client::TlsClientOptions& value) {
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        const auto result =
            client::TlsClientStream::dial("127.0.0.1", port_, value, deadline);
        if (!result.ok()) return false;
        client::EdgeClientConnection wire(result.stream);
        const std::vector<std::byte> payload{std::byte{42}};
        if (!wire.send_frame(payload, deadline)) return false;
        return wire.receive_frame(deadline) == payload;
    }
    std::unique_ptr<IMessageTransport> server_;
    std::uint16_t port_{0};
    std::atomic_int opened_{0};
    std::atomic_int messages_{0};
    std::jthread worker_;
};
TEST_F(MutualTlsTest, DedicatedCaAndExactBothPeerIdentitiesAllowFrames) {
    ASSERT_TRUE(roundtrip(options()));
    EXPECT_EQ(opened_, 1);
    EXPECT_EQ(messages_, 1);
}
TEST_F(MutualTlsTest, MissingClientCertificateCannotReachService) {
    auto value = options();
    value.certificate_chain_file.clear();
    value.private_key_file.clear();
    EXPECT_FALSE(roundtrip(value));
    EXPECT_EQ(opened_, 0);
    EXPECT_EQ(messages_, 0);
}
TEST_F(MutualTlsTest, OtherRoleSignedBySameCaCannotReachService) {
    auto value = options();
    value.certificate_chain_file = identity("other.pem");
    value.private_key_file = identity("other.key");
    EXPECT_FALSE(roundtrip(value));
    EXPECT_EQ(opened_, 0);
    EXPECT_EQ(messages_, 0);
}
TEST_F(MutualTlsTest, WrongCaOrServerIdentityCannotAuthenticateServer) {
    auto value = options();
    value.ca_file = REALMMESH_TEST_TLS_CERTIFICATE;
    EXPECT_FALSE(roundtrip(value));
    value = options();
    value.server_identity = "other-dev-01.other.service";
    EXPECT_FALSE(roundtrip(value));
    EXPECT_EQ(messages_, 0);
}
TEST_F(MutualTlsTest, ClientCertificateFromDifferentCaCannotReachService) {
    auto value = options();
    value.certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE;
    value.private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY;
    EXPECT_FALSE(roundtrip(value));
    EXPECT_EQ(opened_, 0);
    EXPECT_EQ(messages_, 0);
}
TEST_F(MutualTlsTest, WrongAlpnCannotReachService) {
    auto value = options();
    value.alpn = "public-edge/1";
    EXPECT_FALSE(roundtrip(value));
    EXPECT_EQ(opened_, 0);
    EXPECT_EQ(messages_, 0);
}
}  // namespace
}  // namespace realm::network
