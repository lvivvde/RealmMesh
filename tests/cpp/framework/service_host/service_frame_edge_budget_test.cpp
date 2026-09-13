#include "realmmesh/service_host/service_frame.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/service_registry.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/queue_number.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/edge_fetch.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/observability/metrics_registry.hpp"
#include "realmmesh/test_support/fake_service_registry.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
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

/// 最小 TLS 测试客户端:连接 + 握手 + 发帧 + 收帧(轮询可读后读取,
/// 粘包/半包由内部缓冲拼接后按长度前缀解码;#45 用它观测 1303 推送
/// 与关闭)。
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
        // 握手后转非阻塞:TLS 1.3 的 post-handshake 消息(如
        // NewSessionTicket)会让阻塞 SSL_read 在无应用数据时继续等,
        // 而应用数据要靠测试线程自己 tick 驱动 —— 非阻塞 + WANT_READ
        // 回轮询才能收发与推帧交替。
        const int flags = ::fcntl(descriptor_, F_GETFL, 0);
        if (flags < 0 ||
            ::fcntl(descriptor_, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::runtime_error("failed to set nonblocking mode");
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

    /// 读取一帧长度前缀消息:轮询可读后 SSL_read,残缺/粘包留待后续
    /// 拼接;超时或对端关闭返回 nullopt。
    [[nodiscard]] std::optional<std::vector<std::byte>> receive(
        std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        for (;;) {
            if (auto frame = take_frame()) {
                return frame;
            }
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) return std::nullopt;
            std::array<::pollfd, 1> fds{{descriptor_, POLLIN, 0}};
            if (::poll(fds.data(), 1, static_cast<int>(remaining)) <= 0) {
                continue;
            }
            std::array<std::byte, 2048> chunk{};
            std::size_t received = 0;
            const int result = SSL_read_ex(
                ssl_.get(), chunk.data(), chunk.size(), &received);
            if (result != 1) {
                const int error = SSL_get_error(ssl_.get(), result);
                if (error == SSL_ERROR_WANT_READ ||
                    error == SSL_ERROR_WANT_WRITE) {
                    continue;  // 记录未完整/内核缓冲未就绪:回轮询
                }
                return std::nullopt;
            }
            pending_.insert(
                pending_.end(),
                chunk.begin(),
                chunk.begin() + static_cast<std::ptrdiff_t>(received));
        }
    }

    /// 等待对端关闭(EOF);宽限到期关闭与降级断开的观测点。
    [[nodiscard]] bool saw_close(std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) return false;
            std::array<::pollfd, 1> fds{{descriptor_, POLLIN, 0}};
            if (::poll(fds.data(), 1, static_cast<int>(remaining)) <= 0) {
                continue;
            }
            std::array<std::byte, 512> chunk{};
            std::size_t received = 0;
            const int result = SSL_read_ex(
                ssl_.get(), chunk.data(), chunk.size(), &received);
            if (result == 1) {
                continue;  // 关闭前的尾包:继续等 EOF
            }
            const int error = SSL_get_error(ssl_.get(), result);
            if (error == SSL_ERROR_WANT_READ ||
                error == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            // 测试语境下服务端关闭等价于任意终态读错误:干净关闭
            // (ZERO_RETURN)与无 close_notify 的断开(SYSCALL,以及
            // OpenSSL 3 归类为 SSL 的 unexpected EOF)都视为已关闭。
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

/// 定结果拉取源(#44 测试桩):每次尝试返回构造期给定的结果。
/// 恒失败(0 耗时)保持 #43 用例终态不漂移;恒成功下一帧即迁
/// handed-off 并还槽。
class FixedOutcomeFetchSource final :
    public game::gateway::EdgeFetchSource {
public:
    explicit FixedOutcomeFetchSource(game::gateway::EdgeFetchOutcome outcome)
        : outcome_(outcome) {}

    [[nodiscard]] game::gateway::EdgeFetchOutcome fetch(
        std::uint64_t) override {
        return outcome_;
    }

private:
    game::gateway::EdgeFetchOutcome outcome_;
};

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

        fetch_source_.emplace(
            game::gateway::EdgeFetchOutcome{false, std::chrono::milliseconds{0}});
        rebuild_frame(*fetch_source_, std::chrono::seconds{5});

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

    /// 以指定拉取源/重试基数重建帧:默认夹具注入恒失败源 + 5s 基数,
    /// 保持 #43 用例确定性;#44/#45 用例按需换源、缩基数、缩宽限或
    /// 注入静态兜底下游。帧尾指标发布断言经成员注册表(#47)。
    void rebuild_frame(
        game::gateway::EdgeFetchSource& source,
        std::chrono::milliseconds retry_base,
        std::chrono::milliseconds handoff_grace = std::chrono::seconds{5},
        std::string downstream_address = "",
        std::uint16_t downstream_port = 0) {
        frame_.reset();
        frame_.emplace(
            "gateway",
            std::move(downstream_address),
            downstream_port,
            64,
            EdgePipelineCaps{.conn_capacity = 4, .fetch_capacity = 2},
            EdgePipelineTuning{retry_base, 3, handoff_grace},
            &source,
            &metrics_);
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
        client_ = client.get();
        return client;
    }

    /// 边推帧边收帧:测试是唯一驱动者,收帧与 tick 交替进行,帧不会
    /// 因「先等状态、后收帧」的间隙丢失;超时返回 nullopt。
    [[nodiscard]] std::optional<std::vector<std::byte>> receive_while_driving(
        std::chrono::milliseconds budget,
        cluster::ServiceResolver* resolver = nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            frame_->tick(*logger_, *runtime_, resolver, &*reporter_);
            if (auto frame = client_->receive(std::chrono::milliseconds{20})) {
                return frame;
            }
        }
        return std::nullopt;
    }

    /// 注册 Realm 服务实例并建立解析器(#45):handoff 端点来源。
    void register_realm_endpoint() {
        cluster::ServiceInstance instance{
            .type = cluster::ServiceType::Realm,
            .instance_id = "realm-test-01",
            .node_id = "test-node",
            .zone = "test-zone",
        };
        network::TransportEndpoint endpoint;
        endpoint.name = "realm_tls_tcp";
        endpoint.protocol = network::TransportProtocol::TlsTcp;
        endpoint.address = "127.0.0.1";
        endpoint.port = 7100;
        instance.endpoints.push_back(endpoint);
        const auto registration = registry_.register_instance(
            instance, std::chrono::seconds(60));
        EXPECT_EQ(registration.status, cluster::RegistryStatus::Success);
        realm_resolver_.emplace(
            registry_,
            cluster::ServiceType::Realm,
            network::TransportProtocol::TlsTcp);
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
    bool drive_until(
        Pred matches,
        std::chrono::milliseconds budget,
        cluster::ServiceResolver* resolver = nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            frame_->tick(*logger_, *runtime_, resolver, &*reporter_);
            if (matches(observed_budget())) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    }

    /// 反复推帧直到帧尾发布的指标文本满足谓词(#47);超时返回 false。
    template <typename Pred>
    bool drive_until_metrics(
        Pred matches,
        std::chrono::milliseconds budget,
        cluster::ServiceResolver* resolver = nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            frame_->tick(*logger_, *runtime_, resolver, &*reporter_);
            if (matches(metrics_.render())) {
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
    std::optional<FixedOutcomeFetchSource> fetch_source_;
    std::optional<cluster::ServiceResolver> realm_resolver_;
    AttachClient* client_{nullptr};
    std::optional<ServiceFrame> frame_;
    observability::MetricsRegistry metrics_;
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

/// 拉取成功(#44):fetching → handed-off,拉取槽即还,conn 保持占用
/// (#45 起签发票据进入宽限,宽限内会话仍在管,收尾行为见 #45 用例)。
TEST_F(ServiceFrameEdgeBudgetTest, FetchSuccessTransitionsToHandedOff) {
    register_realm_endpoint();
    FixedOutcomeFetchSource source(
        game::gateway::EdgeFetchOutcome{true, std::chrono::milliseconds{0}});
    rebuild_frame(source, std::chrono::seconds{2});

    const auto client = attach("aaaa000000000006aaaa000000000006");
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 3 &&
                   budget->fetch_free == 2;
        },
        std::chrono::seconds{2},
        &*realm_resolver_));
    EXPECT_EQ(
        *observed_budget(),
        InstanceBudgetSnapshot({3, 2, true}));
}

/// handoff 签发(#45):拉取成功即推送 EnterRealmGranted,票据可按
/// EnterRealm 用途兑换出账号 claims,端点指向发现到的 realm;宽限
/// (1s,留足满载下收帧余量)到期未迁移由帧头关闭,conn 预算归还。
TEST_F(ServiceFrameEdgeBudgetTest, GrantIssuedThenGraceCloses) {
    register_realm_endpoint();
    FixedOutcomeFetchSource source(
        game::gateway::EdgeFetchOutcome{true, std::chrono::milliseconds{0}});
    rebuild_frame(
        source, std::chrono::seconds{2}, std::chrono::milliseconds{1000});

    const auto client = attach("aaaa000000000008aaaa000000000008");

    // 1302 attach accepted 在前,1303 grant 在后(服务器主动推送,
    // request_id 恒 0);两帧都经「边推帧边收」取得,不受宽限影响。
    const auto accepted_payload =
        receive_while_driving(std::chrono::seconds{2}, &*realm_resolver_);
    ASSERT_TRUE(accepted_payload.has_value());
    const auto accepted =
        game::common::decode_edge_attach_accepted(*accepted_payload);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->account_id(), 42U);

    const auto granted_payload =
        receive_while_driving(std::chrono::seconds{2}, &*realm_resolver_);
    ASSERT_TRUE(granted_payload.has_value());
    const auto granted =
        game::common::decode_enter_realm_granted(*granted_payload);
    ASSERT_TRUE(granted.has_value());
    ASSERT_EQ(granted->realm_endpoints_size(), 1);
    EXPECT_EQ(granted->realm_endpoints(0).address(), "127.0.0.1");
    EXPECT_EQ(granted->realm_endpoints(0).port(), 7100);
    EXPECT_EQ(
        granted->realm_endpoints(0).protocol(),
        ::realmmesh::protocol::edge::v1::TRANSPORT_PROTOCOL_TLS_TCP);
    game::common::SessionTickets verifier(game::common::parse_ticket_key_hex(
        "0102030405060708090a0b0c0d0e0f10"
        "1112131415161718191a1b1c1d1e1f20"));
    const auto redeemed = verifier.redeem(
        game::common::protobuf_bytes(granted->enter_realm_ticket()),
        game::common::TicketPurpose::EnterRealm);
    ASSERT_EQ(redeemed.status, game::common::RedeemStatus::Accepted);
    EXPECT_EQ(redeemed.claims.account_id, 42U);
    EXPECT_EQ(redeemed.claims.realm_id, 1U);
    EXPECT_EQ(redeemed.claims.character_id, 0U);

    // 签发后拉取槽即还、conn 保持占用:{3,2}。
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 3 &&
                   budget->fetch_free == 2;
        },
        std::chrono::seconds{2},
        &*realm_resolver_));

    // 宽限到期:帧头关闭会话,conn 预算归还 {4,2}。
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 4 &&
                   budget->fetch_free == 2;
        },
        std::chrono::seconds{3},
        &*realm_resolver_));
    EXPECT_TRUE(client->saw_close(std::chrono::seconds{2}));
}

/// 降级路径(#45):发现缺失且未配置静态下游 → 不签发票据,告警后
/// 直接断开,双预算归还。
TEST_F(ServiceFrameEdgeBudgetTest, NoEndpointClosesWithoutGrant) {
    FixedOutcomeFetchSource source(
        game::gateway::EdgeFetchOutcome{true, std::chrono::milliseconds{0}});
    rebuild_frame(source, std::chrono::seconds{2});

    const auto client = attach("aaaa000000000009aaaa000000000009");
    // attach 先消耗一个拉取槽({3,1}),确认会话在管;归还要以
    // 「曾消耗」为前提,否则谓词与初始预算重合、首 tick 即假通过。
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 3 &&
                   budget->fetch_free == 1;
        },
        std::chrono::seconds{2}));
    // 拉取成功但无端点可签发:告警 + 断开,双预算归还 {4,2}。
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 4 &&
                   budget->fetch_free == 2;
        },
        std::chrono::seconds{2}));
    EXPECT_TRUE(client->saw_close(std::chrono::seconds{2}));
}

/// 静态兜底下游(#45):无 Realm 解析器(发现关闭)时,handoff 回退
/// 帧构造注入的静态下游,仍下发 1303 —— 用户故事「无 etcd 拓扑也能
/// 走通」。
TEST_F(ServiceFrameEdgeBudgetTest, StaticDownstreamGrantsWithoutDiscovery) {
    FixedOutcomeFetchSource source(
        game::gateway::EdgeFetchOutcome{true, std::chrono::milliseconds{0}});
    rebuild_frame(
        source,
        std::chrono::seconds{2},
        std::chrono::seconds{5},
        "127.0.0.1",
        7100);

    const auto client = attach("aaaa000000000010aaaa000000000010");
    // 1302 在前,1303 在后;本用例只关心 1303 的端点内容。
    ASSERT_TRUE(receive_while_driving(std::chrono::seconds{2}).has_value());
    const auto granted_payload =
        receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(granted_payload.has_value());
    const auto granted =
        game::common::decode_enter_realm_granted(*granted_payload);
    ASSERT_TRUE(granted.has_value());
    ASSERT_EQ(granted->realm_endpoints_size(), 1);
    EXPECT_EQ(granted->realm_endpoints(0).address(), "127.0.0.1");
    EXPECT_EQ(granted->realm_endpoints(0).port(), 7100);

    // 签发后进入宽限:conn 保持占用,不降级断开。
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 3 &&
                   budget->fetch_free == 2;
        },
        std::chrono::seconds{2}));
    EXPECT_EQ(*observed_budget(), InstanceBudgetSnapshot({3, 2, true}));
}

/// 重试耗尽(#44):基数 30ms 连挂 4 次(首发 + 3 次重试)后上报耗尽,
/// 调用方断开会话 —— conn 与 fetch 双预算随 #43 关闭路径归还。
TEST_F(ServiceFrameEdgeBudgetTest, ExhaustedFetchClosesAndReturnsBudgets) {
    FixedOutcomeFetchSource source(
        game::gateway::EdgeFetchOutcome{false, std::chrono::milliseconds{0}});
    rebuild_frame(source, std::chrono::milliseconds{30});

    const auto client = attach("aaaa000000000007aaaa000000000007");
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->fetch_free == 1;
        },
        std::chrono::seconds{2}));
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 4 &&
                   budget->fetch_free == 2;
        },
        std::chrono::seconds{2}));
    EXPECT_EQ(
        *observed_budget(),
        InstanceBudgetSnapshot({4, 2, true}));
}

/// 帧尾指标发布(#47):慢拉取源下会话可见 fetching 水位与 fetch_free
/// 消耗;成功交付后迁 handed-off,拉取耗时进直方图,重试计数为 0。
TEST_F(ServiceFrameEdgeBudgetTest, MetricsFollowPipelineStagesAndFetch) {
    register_realm_endpoint();
    FixedOutcomeFetchSource source(
        game::gateway::EdgeFetchOutcome{true, std::chrono::milliseconds{600}});
    rebuild_frame(source, std::chrono::seconds{2});

    // attach 前:空管线,三阶段 gauge 全 0,双预算满 {4,2}。
    frame_->tick(*logger_, *runtime_, nullptr, &*reporter_);
    const auto idle = metrics_.render();
    EXPECT_NE(idle.find("edge_sessions{stage=\"pending\"} 0\n"),
              std::string::npos);
    EXPECT_NE(idle.find("edge_sessions{stage=\"fetching\"} 0\n"),
              std::string::npos);
    EXPECT_NE(idle.find("edge_sessions{stage=\"handed_off\"} 0\n"),
              std::string::npos);
    EXPECT_NE(idle.find("edge_budget{kind=\"conn_free\"} 4\n"),
              std::string::npos);
    EXPECT_NE(idle.find("edge_budget{kind=\"fetch_free\"} 2\n"),
              std::string::npos);
    EXPECT_NE(idle.find("edge_fetch_retry_total 0\n"), std::string::npos);

    // attach 受理后进入 fetching:fetching 水位 1、fetch_free 1。
    const auto client = attach("aaaa000000000011aaaa000000000011");
    ASSERT_TRUE(drive_until_metrics(
        [](const std::string& text) {
            return text.find("edge_sessions{stage=\"fetching\"} 1\n") !=
                   std::string::npos &&
                   text.find("edge_budget{kind=\"fetch_free\"} 1\n") !=
                   std::string::npos;
        },
        std::chrono::seconds{2},
        &*realm_resolver_));

    // 交付后迁 handed-off:fetching 回 0,handed_off 1,fetch_free 回满;
    // 600ms 尝试耗时进直方图(le=1 档 1 个,sum 0.6)。
    ASSERT_TRUE(drive_until_metrics(
        [](const std::string& text) {
            return text.find("edge_sessions{stage=\"handed_off\"} 1\n") !=
                   std::string::npos &&
                   text.find("edge_fetch_duration_seconds_count 1\n") !=
                   std::string::npos;
        },
        std::chrono::seconds{2},
        &*realm_resolver_));
    const auto delivered = metrics_.render();
    EXPECT_NE(delivered.find("edge_sessions{stage=\"fetching\"} 0\n"),
              std::string::npos);
    EXPECT_NE(delivered.find("edge_budget{kind=\"fetch_free\"} 2\n"),
              std::string::npos);
    EXPECT_NE(
        delivered.find("edge_fetch_duration_seconds_bucket{le=\"1\"} 1\n"),
        std::string::npos);
    EXPECT_NE(delivered.find("edge_fetch_duration_seconds_sum 0.6\n"),
              std::string::npos);
}

/// 重试与重放计数(#47):30ms 基数耗尽(3 次重试)计入
/// edge_fetch_retry_total;同 jti 新连接重放被拒后计入
/// edge_jti_replay_rejected_total。
TEST_F(ServiceFrameEdgeBudgetTest, MetricsCountRetriesAndReplayRejections) {
    FixedOutcomeFetchSource source(
        game::gateway::EdgeFetchOutcome{false, std::chrono::milliseconds{0}});
    rebuild_frame(source, std::chrono::milliseconds{30});

    const auto first = attach("aaaa000000000012aaaa000000000012");
    ASSERT_TRUE(drive_until_metrics(
        [](const std::string& text) {
            return text.find("edge_fetch_retry_total 3\n") !=
                   std::string::npos;
        },
        std::chrono::seconds{2}));
    // 耗尽断开后双预算归还。
    ASSERT_TRUE(drive_until_metrics(
        [](const std::string& text) {
            return text.find("edge_budget{kind=\"conn_free\"} 4\n") !=
                   std::string::npos &&
                   text.find("edge_budget{kind=\"fetch_free\"} 2\n") !=
                   std::string::npos;
        },
        std::chrono::seconds{2}));

    // 同 jti 新连接重放:凭据无效拒绝,重放计数 1。
    const auto replayed = attach("aaaa000000000012aaaa000000000012");
    ASSERT_TRUE(drive_until_metrics(
        [](const std::string& text) {
            return text.find("edge_jti_replay_rejected_total 1\n") !=
                   std::string::npos;
        },
        std::chrono::seconds{2}));
}

}  // namespace
}  // namespace realm::service_host
