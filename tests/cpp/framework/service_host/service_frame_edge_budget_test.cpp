#include "realmmesh/service_host/service_frame.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/service_registry.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/queue_number.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/test_support/fake_service_registry.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace realm::service_host {
namespace {

using cluster::InstanceBudgetSnapshot;

constexpr std::string_view identity_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view number_seed_hex =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";

/// 签名种子/TLS/票据环境守护:指向 CMake 预生成的自签证书与固定测试
/// 密钥,并注入 attach 验签种子;析构时全部还原(整个 target 串行运行)。
class ScopedEdgeEnvironment final {
public:
    ScopedEdgeEnvironment() {
        EXPECT_EQ(::setenv(
                      "REALMMESH_TLS_CERTIFICATE_FILE",
                      REALMMESH_TEST_TLS_CERTIFICATE,
                      1),
                  0);
        EXPECT_EQ(::setenv(
                      "REALMMESH_TLS_PRIVATE_KEY_FILE",
                      REALMMESH_TEST_TLS_PRIVATE_KEY,
                      1),
                  0);
        EXPECT_EQ(::setenv(
                      "REALMMESH_SESSION_TICKET_KEY",
                      "0102030405060708090a0b0c0d0e0f10"
                      "1112131415161718191a1b1c1d1e1f20",
                      1),
                  0);
        EXPECT_EQ(::setenv(
                      "REALMMESH_IDENTITY_KEY_SEED",
                      identity_seed_hex.data(),
                      1),
                  0);
        EXPECT_EQ(
            ::setenv("REALMMESH_QUEUE_KEY_SEED", number_seed_hex.data(), 1),
            0);
    }
    ~ScopedEdgeEnvironment() {
        static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
        static_cast<void>(::unsetenv("REALMMESH_IDENTITY_KEY_SEED"));
        static_cast<void>(::unsetenv("REALMMESH_QUEUE_KEY_SEED"));
    }
};

struct ContextDeleter {
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};
struct SslDeleter {
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

/// 最小 TLS 测试客户端:连接 + 握手 + 发帧(不含接收,响应由断言侧
/// 通过额度快照间接观测)。
class AttachClient final {
public:
    explicit AttachClient(std::uint16_t port)
        : descriptor_(::socket(AF_INET, SOCK_STREAM, 0)),
          context_(SSL_CTX_new(TLS_client_method())) {
        if (descriptor_ < 0 || !context_) {
            throw std::runtime_error("failed to create TLS test client");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(
                descriptor_,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) < 0) {
            throw std::runtime_error("failed to connect TLS test client");
        }
        if (SSL_CTX_load_verify_locations(
                context_.get(), REALMMESH_TEST_TLS_CERTIFICATE, nullptr) != 1) {
            throw std::runtime_error("failed to load test CA");
        }
        SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER, nullptr);
        ssl_.reset(SSL_new(context_.get()));
        const std::array<unsigned char, 17> alpn{
            16,
            'r',
            'e',
            'a',
            'l',
            'm',
            'm',
            'e',
            's',
            'h',
            '-',
            'e',
            'd',
            'g',
            'e',
            '/',
            '1'};
        if (!ssl_ || SSL_set_fd(ssl_.get(), descriptor_) != 1 ||
            SSL_set_tlsext_host_name(ssl_.get(), "localhost") != 1 ||
            SSL_set1_host(ssl_.get(), "localhost") != 1 ||
            SSL_set_alpn_protos(ssl_.get(), alpn.data(), alpn.size()) != 0 ||
            SSL_connect(ssl_.get()) != 1) {
            throw std::runtime_error("TLS test handshake failed");
        }
    }

    ~AttachClient() {
        if (descriptor_ >= 0) static_cast<void>(::close(descriptor_));
    }
    AttachClient(const AttachClient&) = delete;
    AttachClient& operator=(const AttachClient&) = delete;

    void send(std::span<const std::byte> bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            std::size_t written = 0;
            if (SSL_write_ex(
                    ssl_.get(),
                    bytes.data() + offset,
                    bytes.size() - offset,
                    &written) != 1) {
                throw std::runtime_error("TLS test write failed");
            }
            offset += written;
        }
    }

private:
    int descriptor_;
    std::unique_ptr<SSL_CTX, ContextDeleter> context_;
    std::unique_ptr<SSL, SslDeleter> ssl_;
};

[[nodiscard]] network::TransportConfig tls_transport() {
    return {
        .name = "client_tls_tcp",
        .protocol = network::TransportProtocol::TlsTcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .max_sessions = 16,
        .max_payload_size = 1024,
        .tls =
            network::TransportConfig::TlsServerIdentity{
                .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
                .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
            },
    };
}

class ServiceFrameEdgeBudgetTest : public ::testing::Test {
protected:
    void SetUp() override {
        environment_.emplace();

        log_directory_.emplace("realmmesh-frame-budget-");
        observability::LoggerConfig logger_config;
        logger_config.file_path = log_directory_->path() / "frame.log";
        logger_.emplace(
            logger_config,
            observability::ServiceIdentity{.service_name = "gateway"});

        cluster::ServiceInstance instance{
            .type = cluster::ServiceType::Gateway,
            .instance_id = instance_id_,
            .node_id = "test-node",
            .zone = "test-zone",
        };
        network::TransportEndpoint endpoint;
        endpoint.name = "client_tls_tcp";
        endpoint.protocol = network::TransportProtocol::TlsTcp;
        endpoint.address = "127.0.0.1";
        endpoint.port = 8443;
        instance.endpoints.push_back(endpoint);
        const auto registration = registry_.register_instance(
            instance, std::chrono::seconds(60));
        ASSERT_EQ(registration.status, cluster::RegistryStatus::Success);
        registration_id_ = registration.id;
        reporter_.emplace(
            registry_,
            registration_id_,
            cluster::ServiceType::Gateway,
            instance_id_);

        runtime_.emplace(
            game::gateway::GatewayConfig{.transports = {tls_transport()}},
            game::gateway::GatewayRuntimeOptions{
                .inbound_capacity = 64,
                .outbound_capacity = 64,
                .io_poll_interval = std::chrono::milliseconds{1}});
        runtime_->start();

        frame_.emplace(
            "gateway",
            "",
            0,
            64,
            EdgePipelineCaps{.conn_capacity = 4, .fetch_capacity = 2});

        identity_codec_.emplace(
            game::common::parse_identity_seed_hex(identity_seed_hex),
            "login-verify-v1");
        number_codec_.emplace(
            game::common::parse_identity_seed_hex(number_seed_hex),
            "queue-v1");
    }

    void TearDown() override {
        frame_.reset();
        runtime_->stop();
        reporter_.reset();
        environment_.reset();
    }

    /// 客户端连接并发送 attach 帧;返回保持连接的客户端(析构即断开)。
    [[nodiscard]] std::unique_ptr<AttachClient> attach(
        std::string_view jti,
        bool admitted = true) {
        auto client = std::make_unique<AttachClient>(
            static_cast<std::uint16_t>(
                runtime_->local_endpoints().front().port));
        game::common::EdgeAttach message;
        message.set_identity_token(identity_token(jti));
        message.set_queue_number_token(number_token(admitted));
        const network::LengthFieldCodec codec(1024);
        client->send(
            codec.encode(game::common::encode(message, 5)));
        return client;
    }

    [[nodiscard]] std::optional<InstanceBudgetSnapshot> observed_budget() {
        const auto leased = registry_.leased_keys(registration_id_);
        const auto found = leased.find(
            cluster::budget_key(cluster::ServiceType::Gateway, instance_id_));
        if (found == leased.end()) {
            return std::nullopt;
        }
        const auto budget = nlohmann::json::parse(found->second);
        return InstanceBudgetSnapshot{
            static_cast<std::uint64_t>(budget["conn_free"]),
            static_cast<std::uint64_t>(budget["fetch_free"]),
            true,
        };
    }

    /// 反复推帧直到额度快照满足谓词;超时返回 false(快照经
    /// observed_budget 读取,由调用方断言终态)。
    template <typename Pred>
    bool drive_until(Pred matches, std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            frame_->tick(*logger_, *runtime_, nullptr, &*reporter_);
            if (matches(observed_budget())) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    }

    std::string instance_id_{"gateway-test-01"};
    cluster::RegistrationId registration_id_{
        cluster::invalid_registration_id};
    test_support::FakeServiceRegistry registry_;
    std::optional<ScopedEdgeEnvironment> environment_;
    std::optional<test_support::TemporaryDirectory> log_directory_;
    std::optional<observability::Logger> logger_;
    std::optional<cluster::InstanceBudgetReporter> reporter_;
    std::optional<game::gateway::GatewayRuntime> runtime_;
    std::optional<ServiceFrame> frame_;
    std::optional<game::common::IdentityTokenCodec> identity_codec_;
    std::optional<game::common::QueueNumberCodec> number_codec_;

private:
    [[nodiscard]] std::string identity_token(std::string_view jti) const {
        const auto now = std::chrono::system_clock::now();
        return identity_codec_->issue(game::common::IdentityClaims{
            .issuer = "realmmesh/login-verify",
            .account_id = 42,
            .jti = std::string(jti),
            .issued_at = now,
            .expires_at = now + std::chrono::minutes{30}});
    }

    [[nodiscard]] std::string number_token(bool admitted) const {
        const auto now = std::chrono::system_clock::now();
        return number_codec_->issue(game::common::QueueNumberClaims{
            .number = 7,
            .admitted = admitted,
            .issued_at = now,
            .expires_at = now + std::chrono::seconds{300}});
    }
};

TEST_F(ServiceFrameEdgeBudgetTest, PublishesCapacityThenConsumesOnAttach) {
    frame_->tick(*logger_, *runtime_, nullptr, &*reporter_);
    const auto initial = observed_budget();
    ASSERT_TRUE(initial.has_value());
    EXPECT_EQ(*initial, InstanceBudgetSnapshot({4, 2, true}));

    const auto client = attach("aaaa000000000001aaaa000000000001");
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->fetch_free == 1;
        },
        std::chrono::seconds{2}));
    EXPECT_EQ(
        *observed_budget(),
        InstanceBudgetSnapshot({3, 1, true}));
}

TEST_F(ServiceFrameEdgeBudgetTest, ReplayedNumberIsRejectedOnNewConnection) {
    const auto first = attach("aaaa000000000002aaaa000000000002");
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->fetch_free == 1;
        },
        std::chrono::seconds{2}));

    // 同一号牌(jti)在新连接重放:凭据无效 → 拒绝并终结,拉取额度
    // 不被消耗(conn 的一次瞬时占用由 SessionClosed 归还)。
    const auto replayed = attach("aaaa000000000002aaaa000000000002");
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 2;
        },
        std::chrono::seconds{2}));
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 3;
        },
        std::chrono::seconds{2}));
    EXPECT_EQ(
        *observed_budget(),
        InstanceBudgetSnapshot({3, 1, true}));
}

TEST_F(ServiceFrameEdgeBudgetTest, OutOfBudgetAttachIsRejectedWithoutConsuming) {
    const auto first = attach("aaaa000000000003aaaa000000000003");
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->fetch_free == 1;
        },
        std::chrono::seconds{2}));
    const auto second = attach("aaaa000000000004aaaa000000000004");
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->fetch_free == 0;
        },
        std::chrono::seconds{2}));

    // 第三个客户端凭据有效但拉取池已满:额度外拒绝,不消耗任何额度
    // (conn 的瞬时占用归还后回到 {2,0})。
    const auto third = attach("aaaa000000000005aaaa000000000005");
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 1;
        },
        std::chrono::seconds{2}));
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 2 &&
                   budget->fetch_free == 0;
        },
        std::chrono::seconds{2}));
    EXPECT_EQ(
        *observed_budget(),
        InstanceBudgetSnapshot({2, 0, true}));
}

}  // namespace
}  // namespace realm::service_host
