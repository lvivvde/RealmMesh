#include "realmmesh/service_host/service_frame.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/service_registry.hpp"
#include "realmmesh/common/v1/envelope.pb.h"
#include "realmmesh/game/common/compact_jws.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/test_support/edge_raw_frame.hpp"
#include "realmmesh/test_support/fake_service_registry.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
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

/// 签名/TLS/票据环境守护(#46 realm 侧只需要 TLS 证书与共享票据键;
/// attach 验签上下文惰性构造,不触碰身份/号牌种子)。
class ScopedRealmEnvironment final {
public:
    ScopedRealmEnvironment() {
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_TLS_CERTIFICATE_FILE",
                REALMMESH_TEST_TLS_CERTIFICATE,
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_TLS_PRIVATE_KEY_FILE",
                REALMMESH_TEST_TLS_PRIVATE_KEY,
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_SESSION_TICKET_KEY",
                "0102030405060708090a0b0c0d0e0f10"
                "1112131415161718191a1b1c1d1e1f20",
                1),
            0);
    }
    ~ScopedRealmEnvironment() {
        static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
    }
};

struct ContextDeleter {
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};
struct SslDeleter {
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

/// 最小 TLS 测试客户端(与 edge budget 测试同款):连接 + 握手 + 发帧 +
/// 收帧;握手后转非阻塞,TLS 1.3 post-handshake 消息不再阻塞轮询循环。
class RealmTestClient final {
public:
    explicit RealmTestClient(std::uint16_t port)
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
        const int flags = ::fcntl(descriptor_, F_GETFL, 0);
        if (flags < 0 ||
            ::fcntl(descriptor_, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::runtime_error("failed to set nonblocking mode");
        }
    }

    ~RealmTestClient() {
        if (descriptor_ >= 0) static_cast<void>(::close(descriptor_));
    }
    RealmTestClient(const RealmTestClient&) = delete;
    RealmTestClient& operator=(const RealmTestClient&) = delete;

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

    [[nodiscard]] std::optional<std::vector<std::byte>> receive(
        std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        for (;;) {
            if (auto frame = take_frame()) {
                return frame;
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now())
                    .count();
            if (remaining <= 0) return std::nullopt;
            std::array<::pollfd, 1> fds{{descriptor_, POLLIN, 0}};
            if (::poll(fds.data(), 1, static_cast<int>(remaining)) <= 0) {
                continue;
            }
            std::array<std::byte, 2048> chunk{};
            std::size_t received = 0;
            const int result =
                SSL_read_ex(ssl_.get(), chunk.data(), chunk.size(), &received);
            if (result != 1) {
                const int error = SSL_get_error(ssl_.get(), result);
                if (error == SSL_ERROR_WANT_READ ||
                    error == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
                return std::nullopt;
            }
            pending_.insert(
                pending_.end(),
                chunk.begin(),
                chunk.begin() + static_cast<std::ptrdiff_t>(received));
        }
    }

    [[nodiscard]] bool saw_close(std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now())
                    .count();
            if (remaining <= 0) return false;
            std::array<::pollfd, 1> fds{{descriptor_, POLLIN, 0}};
            if (::poll(fds.data(), 1, static_cast<int>(remaining)) <= 0) {
                continue;
            }
            std::array<std::byte, 512> chunk{};
            std::size_t received = 0;
            const int result =
                SSL_read_ex(ssl_.get(), chunk.data(), chunk.size(), &received);
            if (result == 1) {
                continue;
            }
            const int error = SSL_get_error(ssl_.get(), result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> take_frame() {
        if (pending_.empty()) return std::nullopt;
        network::ByteBuffer buffer;
        buffer.append(pending_);
        const network::LengthFieldCodec codec(1024);
        const auto result = codec.try_decode(buffer);
        if (result.status != network::DecodeStatus::FrameReady) {
            return std::nullopt;
        }
        const auto consumed = pending_.size() - buffer.readable_bytes();
        pending_.erase(
            pending_.begin(),
            pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
        return result.payload;
    }

private:
    int descriptor_;
    std::unique_ptr<SSL_CTX, ContextDeleter> context_;
    std::unique_ptr<SSL, SslDeleter> ssl_;
    std::vector<std::byte> pending_;
};

[[nodiscard]] network::TransportConfig tls_transport() {
    return {
        .name = "realm_tls_tcp",
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

class ServiceFrameRealmEnterTest : public ::testing::Test {
protected:
    void SetUp() override {
        environment_.emplace();

        log_directory_.emplace("realmmesh-frame-realm-enter-");
        observability::LoggerConfig logger_config;
        logger_config.file_path = log_directory_->path() / "frame.log";
        logger_.emplace(
            logger_config,
            observability::ServiceIdentity{.service_name = "realm"});

        cluster::ServiceInstance instance{
            .type = cluster::ServiceType::Realm,
            .instance_id = instance_id_,
            .node_id = "test-node",
            .zone = "test-zone",
        };
        network::TransportEndpoint endpoint;
        endpoint.name = "realm_tls_tcp";
        endpoint.protocol = network::TransportProtocol::TlsTcp;
        endpoint.address = "127.0.0.1";
        endpoint.port = 7100;
        instance.endpoints.push_back(endpoint);
        const auto registration =
            registry_.register_instance(instance, std::chrono::seconds(60));
        ASSERT_EQ(registration.status, cluster::RegistryStatus::Success);
        registration_id_ = registration.id;
        reporter_.emplace(
            registry_,
            registration_id_,
            cluster::ServiceType::Realm,
            instance_id_);

        runtime_.emplace(
            game::gateway::GatewayConfig{.transports = {tls_transport()}},
            game::gateway::GatewayRuntimeOptions{
                .inbound_capacity = 64,
                .outbound_capacity = 64,
                .io_poll_interval = std::chrono::milliseconds{1}});
        runtime_->start();

        frame_.emplace("realm", "127.0.0.1", 8443, 64, 4);

        tickets_.emplace(
            game::common::parse_ticket_key_hex(
                "0102030405060708090a0b0c0d0e0f10"
                "1112131415161718191a1b1c1d1e1f20"));
    }

    void TearDown() override {
        frame_.reset();
        runtime_->stop();
        reporter_.reset();
        environment_.reset();
    }

    /// 客户端连接(仅建立 TLS,不发送任何消息);析构即断开。
    [[nodiscard]] std::unique_ptr<RealmTestClient> connect() {
        return std::make_unique<RealmTestClient>(static_cast<std::uint16_t>(
            runtime_->local_endpoints().front().port));
    }

    /// 以测试侧共享密钥签发 EnterRealm 票据(帧侧 load_ticket_key 同键)。
    [[nodiscard]] std::vector<std::byte> enter_ticket(
        std::uint64_t account_id,
        std::uint32_t realm_id = 1,
        std::chrono::seconds ttl = std::chrono::seconds{60},
        std::chrono::system_clock::time_point issued_at =
            std::chrono::system_clock::now()) {
        return tickets_->issue(
            game::common::TicketPurpose::EnterRealm,
            account_id,
            realm_id,
            0,
            ttl,
            issued_at);
    }

    /// 发送 1304 兑换请求帧。
    void submit_enter_realm(
        RealmTestClient& client,
        std::span<const std::byte> ticket,
        std::uint64_t request_id = 7) {
        game::common::EnterRealm request;
        request.set_enter_realm_ticket(
            reinterpret_cast<const char*>(ticket.data()), ticket.size());
        const network::LengthFieldCodec codec(1024);
        client.send(codec.encode(game::common::encode(request, request_id)));
    }

    /// 边推帧边收帧:测试是唯一驱动者,收帧与 tick 交替,帧不丢失。
    [[nodiscard]] std::optional<std::vector<std::byte>> receive_while_driving(
        std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            frame_->tick(*logger_, *runtime_, nullptr, &*reporter_);
            if (auto frame = client_->receive(std::chrono::milliseconds{20})) {
                return frame;
            }
        }
        return std::nullopt;
    }

    /// 读取 realm 额度 key 的已发布 JSON;无 fetch_free 键即
    /// has_fetch=false(§5.2:realm 仅 conn_free)。
    [[nodiscard]] std::optional<InstanceBudgetSnapshot> observed_budget()
        const {
        const auto leased = registry_.leased_keys(registration_id_);
        const auto found = leased.find(
            cluster::budget_key(cluster::ServiceType::Realm, instance_id_));
        if (found == leased.end()) {
            return std::nullopt;
        }
        const auto budget = nlohmann::json::parse(found->second);
        EXPECT_FALSE(budget.contains("fetch_free"));
        return InstanceBudgetSnapshot{
            static_cast<std::uint64_t>(budget["conn_free"]),
            0,
            false,
        };
    }

    /// 反复推帧直到额度快照满足谓词;超时返回 false。
    template <typename Pred>
    bool drive_until_budget(Pred matches, std::chrono::milliseconds budget) {
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

    std::string instance_id_{"realm-test-01"};
    cluster::RegistrationId registration_id_{cluster::invalid_registration_id};
    test_support::FakeServiceRegistry registry_;
    std::optional<ScopedRealmEnvironment> environment_;
    std::optional<test_support::TemporaryDirectory> log_directory_;
    std::optional<observability::Logger> logger_;
    std::optional<cluster::InstanceBudgetReporter> reporter_;
    std::optional<game::gateway::GatewayRuntime> runtime_;
    std::optional<game::common::SessionTickets> tickets_;
    RealmTestClient* client_{nullptr};
    std::optional<ServiceFrame> frame_;
};

/// 兑换成功(#46):1304 → 1305 受理并迁入 established,会话心跳可用。
TEST_F(ServiceFrameRealmEnterTest, EnterRealmRedeemsTicketAndAcceptsSession) {
    const auto ticket = enter_ticket(42);
    auto client = connect();
    client_ = client.get();
    submit_enter_realm(*client, ticket, 7);

    const auto granted = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(granted.has_value());
    const auto accepted = game::common::decode_enter_realm_accepted(*granted);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(
        game::common::edge_message_id(*granted),
        game::common::EdgeMessageId::MESSAGE_ID_S2C_ENTER_REALM_ACCEPTED);
    EXPECT_EQ(game::common::edge_request_id(*granted), 7U);
    EXPECT_EQ(accepted->account_id(), 42U);

    // 已入场会话心跳可用:established + authenticated 语义生效。
    game::common::HeartbeatRequest heartbeat;
    const network::LengthFieldCodec codec(1024);
    client->send(codec.encode(game::common::encode(heartbeat, 9)));
    const auto beat = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(beat.has_value());
    EXPECT_TRUE(game::common::decode_heartbeat_response(*beat).has_value());
}

/// 一次性消费(#46):同票据第二连接兑换被拒(回放),先入场会话不受影响。
TEST_F(ServiceFrameRealmEnterTest, EnterRealmTicketIsSingleConsume) {
    const auto ticket = enter_ticket(42);
    auto first = connect();
    client_ = first.get();
    submit_enter_realm(*first, ticket, 7);
    const auto accepted = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(
        game::common::decode_enter_realm_accepted(accepted.value())
            .has_value());

    auto second = connect();
    submit_enter_realm(*second, ticket, 8);
    std::optional<std::vector<std::byte>> replayed;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        frame_->tick(*logger_, *runtime_, nullptr, &*reporter_);
        if (auto frame = second->receive(std::chrono::milliseconds{20})) {
            replayed = std::move(frame);
            break;
        }
    }
    ASSERT_TRUE(replayed.has_value());
    const auto error = game::common::decode_edge_error(*replayed);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(
        error->code(), game::common::edge_error_invalid_enter_realm_ticket);
    EXPECT_TRUE(second->saw_close(std::chrono::seconds{2}));

    // 先入场会话不因他人重放受影响:心跳仍应答。
    game::common::HeartbeatRequest heartbeat;
    const network::LengthFieldCodec codec(1024);
    first->send(codec.encode(game::common::encode(heartbeat, 9)));
    client_ = first.get();
    const auto beat = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(beat.has_value());
    EXPECT_TRUE(game::common::decode_heartbeat_response(*beat).has_value());
}

/// 已入场会话重复提交 1304:同连接回放同样被拒并关闭(回包后终结)。
TEST_F(ServiceFrameRealmEnterTest, ResubmissionOnSameSessionCloses) {
    const auto ticket = enter_ticket(42);
    auto client = connect();
    client_ = client.get();
    submit_enter_realm(*client, ticket, 7);
    const auto accepted = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(
        game::common::decode_enter_realm_accepted(accepted.value())
            .has_value());

    submit_enter_realm(*client, ticket, 8);
    std::optional<std::vector<std::byte>> resubmitted;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        frame_->tick(*logger_, *runtime_, nullptr, &*reporter_);
        if (auto frame = client->receive(std::chrono::milliseconds{20})) {
            resubmitted = std::move(frame);
            break;
        }
    }
    ASSERT_TRUE(resubmitted.has_value());
    const auto error = game::common::decode_edge_error(*resubmitted);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(
        error->code(), game::common::edge_error_invalid_enter_realm_ticket);
    EXPECT_TRUE(client->saw_close(std::chrono::seconds{2}));
}

/// 无效票据(构造合法但验签不过的字节)→ 3002 + 未入会话终结。
TEST_F(ServiceFrameRealmEnterTest, InvalidTicketBytesDecline) {
    auto client = connect();
    client_ = client.get();
    submit_enter_realm(*client, std::vector<std::byte>(32, std::byte{0xAB}), 7);

    const auto response = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(response.has_value());
    const auto error = game::common::decode_edge_error(*response);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(
        error->code(), game::common::edge_error_invalid_enter_realm_ticket);
    EXPECT_TRUE(client->saw_close(std::chrono::seconds{2}));
}

/// 已退役编号(#50):1101 曾在旧链承载 Realm 认证,新代码在解码层不再
/// 识别它 —— 未入场会话发它落未认证/拒绝,而不是被当成业务消息处理。
/// request_id 随编号一起解不出,回包恒 0。
TEST_F(ServiceFrameRealmEnterTest, RetiredMessageIdIsRefused) {
    auto client = connect();
    client_ = client.get();
    const network::LengthFieldCodec codec(1024);
    client->send(codec.encode(test_support::edge_raw_frame(1101, 11)));

    const auto response = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(response.has_value());
    const auto error = game::common::decode_edge_error(*response);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code(), game::common::edge_error_not_authenticated);
    EXPECT_EQ(game::common::edge_request_id(*response), 0U);
    EXPECT_TRUE(client->saw_close(std::chrono::seconds{2}));
}

/// 未入场会话只受理 1304(#50 后不变):编号仍现役的心跳在入场前同样
/// 落未认证/拒绝分支,不得被当成业务消息处理。
TEST_F(ServiceFrameRealmEnterTest, MessageBeforeEnterRealmIsRefused) {
    auto client = connect();
    client_ = client.get();
    game::common::HeartbeatRequest heartbeat;
    const network::LengthFieldCodec codec(1024);
    client->send(codec.encode(game::common::encode(heartbeat, 12)));

    const auto response = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(response.has_value());
    const auto error = game::common::decode_edge_error(*response);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code(), game::common::edge_error_not_authenticated);
    EXPECT_EQ(game::common::edge_request_id(*response), 12U);
    EXPECT_TRUE(client->saw_close(std::chrono::seconds{2}));
}

/// 用途不符(已退役的入场票据用途数值 2)→ 3002,不烧票不迁移。
/// 枚举里已无该取值,只能用整数构造:退役数值不得被当作活用途接受。
TEST_F(ServiceFrameRealmEnterTest, WrongPurposeTicketDeclines) {
    const auto ticket = tickets_->issue(
        static_cast<game::common::TicketPurpose>(2),
        42,
        1,
        7,
        std::chrono::seconds{60});
    auto client = connect();
    client_ = client.get();
    submit_enter_realm(*client, ticket, 7);

    const auto response = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(response.has_value());
    const auto error = game::common::decode_edge_error(*response);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(
        error->code(), game::common::edge_error_invalid_enter_realm_ticket);
    EXPECT_TRUE(client->saw_close(std::chrono::seconds{2}));
}

/// realm 声明不符(≠1)→ 3002:redeem 侧的 realm 不变量。
TEST_F(ServiceFrameRealmEnterTest, ForeignRealmClaimDeclines) {
    const auto ticket = enter_ticket(42, 2);
    auto client = connect();
    client_ = client.get();
    submit_enter_realm(*client, ticket, 7);

    const auto response = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(response.has_value());
    const auto error = game::common::decode_edge_error(*response);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(
        error->code(), game::common::edge_error_invalid_enter_realm_ticket);
    EXPECT_TRUE(client->saw_close(std::chrono::seconds{2}));
}

/// 过期票据 → 3002。过期判定含跨机时钟容差(now ≤ expires_at + leeway),
/// 所以「过期」必须越过容差窗口才算:签发于 60s 期限 + 容差 + 1s 之前。
TEST_F(ServiceFrameRealmEnterTest, ExpiredTicketDeclines) {
    constexpr auto ttl = std::chrono::seconds{60};
    const auto ticket = enter_ticket(
        42,
        1,
        ttl,
        std::chrono::system_clock::now() -
            (ttl + game::common::jws_clock_leeway + std::chrono::seconds{1}));
    auto client = connect();
    client_ = client.get();
    submit_enter_realm(*client, ticket, 7);

    const auto response = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(response.has_value());
    const auto error = game::common::decode_edge_error(*response);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(
        error->code(), game::common::edge_error_invalid_enter_realm_ticket);
    EXPECT_TRUE(client->saw_close(std::chrono::seconds{2}));
}

/// conn_free 额度上报(#46):首帧发布容量;入场扣减;断开归还。
/// fetch 维度缺席(§5.2:realm 仅 conn_free)。
TEST_F(ServiceFrameRealmEnterTest, ConnFreeBudgetTracksConnections) {
    frame_->tick(*logger_, *runtime_, nullptr, &*reporter_);
    const auto initial = observed_budget();
    ASSERT_TRUE(initial.has_value());
    EXPECT_EQ(*initial, InstanceBudgetSnapshot({4, 0, false}));

    const auto ticket = enter_ticket(42);
    auto client = connect();
    client_ = client.get();
    submit_enter_realm(*client, ticket, 7);
    const auto accepted = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(
        game::common::decode_enter_realm_accepted(accepted.value())
            .has_value());
    EXPECT_TRUE(drive_until_budget(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 3;
        },
        std::chrono::seconds{2}));

    client.reset();
    EXPECT_TRUE(drive_until_budget(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 4;
        },
        std::chrono::seconds{2}));
}

}  // namespace
}  // namespace realm::service_host
