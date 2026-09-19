#include "realmmesh/service_host/service_frame.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/service_registry.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/common/v1/envelope.pb.h"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/queue_number.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/account_fetch_port.hpp"
#include "realmmesh/game/gateway/gateway_login_config.hpp"
#include "realmmesh/game/gateway/gateway_login_pipeline.hpp"
#include "realmmesh/game/gateway/gateway_primary_transport.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/observability/metrics_registry.hpp"
#include "realmmesh/test_support/edge_raw_frame.hpp"
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
#include <fstream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realm::service_host {
namespace {

using cluster::InstanceBudgetSnapshot;

constexpr std::string_view identity_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view number_seed_hex =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
constexpr std::string_view ticket_key_hex =
    "0102030405060708090a0b0c0d0e0f10"
    "1112131415161718191a1b1c1d1e1f20";

[[nodiscard]] game::gateway::GatewaySigningMaterial signing_material() {
    return game::gateway::GatewaySigningMaterial::from_hex(
        identity_seed_hex,
        "login-verify-v1",
        number_seed_hex,
        "queue-v1",
        ticket_key_hex,
        "realmmesh/login-verify");
}

/// 签名种子/TLS/票据环境守护:指向 CMake 预生成的自签证书与固定测试
/// 密钥,并注入 attach 验签种子;析构时全部还原(整个 target 串行运行)。
/// #75 只描述签名材料可用时的兼容行为;首个 attach 才惰性装载材料是
/// #74 已批准修正,不得由本夹具固化为启动契约。
class ScopedEdgeEnvironment final {
public:
    ScopedEdgeEnvironment() {
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
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_IDENTITY_KEY_SEED", identity_seed_hex.data(), 1),
            0);
        EXPECT_EQ(
            ::setenv("REALMMESH_QUEUE_KEY_SEED", number_seed_hex.data(), 1), 0);
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
                continue;  // 关闭前的尾包:继续等 EOF
            }
            const int error = SSL_get_error(ssl_.get(), result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
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

/// 定结果非阻塞拉取适配器：submit 只登记 attempt，完成结果到期后
/// 由 drain_completions 交付，测试不会在业务帧内执行同步拉取。
class FixedOutcomeAccountFetchPort final
    : public game::gateway::AccountFetchPort {
public:
    FixedOutcomeAccountFetchPort(bool ok, std::chrono::milliseconds duration)
        : ok_(ok),
          duration_(duration) {}

    [[nodiscard]] game::gateway::AccountFetchSubmitResult submit(
        game::gateway::AccountFetchRequest request,
        std::chrono::steady_clock::time_point now) override {
        if (stopped_) {
            return game::gateway::AccountFetchSubmitResult::Stopped;
        }
        pending_.insert_or_assign(request.attempt_id.value, now + duration_);
        return game::gateway::AccountFetchSubmitResult::Submitted;
    }

    [[nodiscard]] std::vector<game::gateway::AccountFetchCompletion>
    drain_completions(
        std::chrono::steady_clock::time_point now,
        std::size_t max_completions) override {
        std::vector<game::gateway::AccountFetchCompletion> completions;
        for (auto entry = pending_.begin();
             entry != pending_.end() && completions.size() < max_completions;) {
            if (entry->second > now) {
                ++entry;
                continue;
            }
            completions.push_back(
                {game::gateway::AccountFetchAttemptId{entry->first},
                 ok_,
                 duration_});
            entry = pending_.erase(entry);
        }
        return completions;
    }

    void cancel(game::gateway::AccountFetchAttemptId attempt_id) override {
        pending_.erase(attempt_id.value);
    }

    void stop() noexcept { stopped_ = true; }

private:
    bool ok_{false};
    std::chrono::milliseconds duration_{0};
    bool stopped_{false};
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point>
        pending_;
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
        const auto registration =
            registry_.register_instance(instance, std::chrono::seconds(60));
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
        rebuild_frame(
            false, std::chrono::milliseconds{0}, std::chrono::seconds{5});
        runtime_->start();

        identity_codec_.emplace(
            game::common::parse_identity_seed_hex(identity_seed_hex),
            "login-verify-v1");
        number_codec_.emplace(
            game::common::parse_identity_seed_hex(number_seed_hex), "queue-v1");
    }

    void TearDown() override {
        frame_.reset();
        pipeline_.reset();
        fetch_port_.reset();
        primary_transport_.reset();
        runtime_->stop();
        reporter_.reset();
        environment_.reset();
    }

    /// 以指定拉取结果/重试基数重建管线与帧:默认夹具注入恒失败 + 5s 基数,
    /// 保持 #43 用例确定性;#44/#45 用例按需换源、缩基数、缩宽限或
    /// 注入静态兜底下游。帧尾指标发布断言经成员注册表(#47)。
    void rebuild_frame(
        bool fetch_ok,
        std::chrono::milliseconds fetch_duration,
        std::chrono::milliseconds retry_base,
        std::chrono::milliseconds handoff_grace = std::chrono::seconds{5},
        std::string downstream_address = "",
        std::uint16_t downstream_port = 0) {
        frame_.reset();
        pipeline_.reset();
        fetch_port_.reset();
        primary_transport_.reset();
        primary_transport_.emplace(*runtime_);
        fetch_port_ = std::make_unique<FixedOutcomeAccountFetchPort>(
            fetch_ok, fetch_duration);
        game::gateway::GatewayLoginConfig login_config{
            .conn_capacity = 4,
            .fetch_capacity = 2,
            .fetch_retry_base = retry_base,
            .fetch_retry_max = 3,
            .handoff_grace = handoff_grace,
        };
        if (!downstream_address.empty() && downstream_port != 0) {
            login_config.static_realm = game::gateway::RealmEndpoint{
                std::move(downstream_address), downstream_port};
        }
        pipeline_.emplace(
            game::gateway::GatewayLoginPipeline::create(
                std::move(login_config),
                signing_material(),
                *primary_transport_,
                *fetch_port_,
                &*logger_,
                &metrics_));
        frame_.emplace("gateway", "", 0, 64, 4, &*pipeline_);
    }

    /// 客户端连接并发送 attach 帧;返回保持连接的客户端(析构即断开)。
    [[nodiscard]] std::unique_ptr<AttachClient> attach_with_tokens(
        std::string identity_token,
        std::string number_token,
        std::uint64_t request_id) {
        auto client = std::make_unique<AttachClient>(static_cast<std::uint16_t>(
            runtime_->local_endpoints().front().port));
        game::common::EdgeAttach message;
        message.set_identity_token(std::move(identity_token));
        message.set_queue_number_token(std::move(number_token));
        const network::LengthFieldCodec codec(1024);
        client->send(codec.encode(game::common::encode(message, request_id)));
        client_ = client.get();
        return client;
    }

    /// 使用固定签名夹具提交 attach;request_id 由用例显式给出,
    /// 使线契约断言不依赖 helper 内部常量。
    [[nodiscard]] std::unique_ptr<AttachClient> attach(
        std::string_view jti,
        bool admitted = true,
        std::uint64_t request_id = 5) {
        return attach_with_tokens(
            identity_token(jti), number_token(admitted), request_id);
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
    void register_realm_endpoint(
        std::string address = "127.0.0.1", std::uint16_t port = 7100) {
        cluster::ServiceInstance instance{
            .type = cluster::ServiceType::Realm,
            .instance_id = "realm-test-01",
            .node_id = "test-node",
            .zone = "test-zone",
        };
        network::TransportEndpoint endpoint;
        endpoint.name = "realm_tls_tcp";
        endpoint.protocol = network::TransportProtocol::TlsTcp;
        endpoint.address = std::move(address);
        endpoint.port = port;
        instance.endpoints.push_back(endpoint);
        const auto registration =
            registry_.register_instance(instance, std::chrono::seconds(60));
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
    cluster::RegistrationId registration_id_{cluster::invalid_registration_id};
    test_support::FakeServiceRegistry registry_;
    std::optional<ScopedEdgeEnvironment> environment_;
    std::optional<test_support::TemporaryDirectory> log_directory_;
    std::optional<observability::Logger> logger_;
    std::optional<cluster::InstanceBudgetReporter> reporter_;
    std::optional<game::gateway::GatewayRuntime> runtime_;
    std::optional<game::gateway::GatewayRuntimePrimaryTransport>
        primary_transport_;
    std::unique_ptr<FixedOutcomeAccountFetchPort> fetch_port_;
    std::optional<game::gateway::GatewayLoginPipeline> pipeline_;
    std::optional<cluster::ServiceResolver> realm_resolver_;
    AttachClient* client_{nullptr};
    std::optional<ServiceFrame> frame_;
    observability::MetricsRegistry metrics_;
    std::optional<game::common::IdentityTokenCodec> identity_codec_;
    std::optional<game::common::QueueNumberCodec> number_codec_;

private:
    [[nodiscard]] std::string identity_token(std::string_view jti) const {
        const auto now = std::chrono::system_clock::now();
        return identity_codec_->issue(
            game::common::IdentityClaims{
                .issuer = "realmmesh/login-verify",
                .account_id = 42,
                .jti = std::string(jti),
                .issued_at = now,
                .expires_at = now + std::chrono::minutes{30}});
    }

    [[nodiscard]] std::string number_token(bool admitted) const {
        const auto now = std::chrono::system_clock::now();
        return number_codec_->issue(
            game::common::QueueNumberClaims{
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
    EXPECT_EQ(*observed_budget(), InstanceBudgetSnapshot({3, 1, true}));
}

/// #75 保留线契约:Gateway 受理 attach 时回 1302、复用请求的
/// request_id,并携带拉取账号。后续 Pipeline 内部如何保留
/// Runtime intent 不应改变这三个外部事实。
TEST_F(ServiceFrameEdgeBudgetTest, AttachAcceptancePreservesWireContract) {
    constexpr std::uint64_t request_id = 0x1020'3040;
    const auto client =
        attach("aaaa000000000013aaaa000000000013", true, request_id);

    const auto response = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(
        game::common::edge_message_id(*response),
        game::common::EdgeMessageId::MESSAGE_ID_S2C_EDGE_ATTACH_ACCEPTED);
    EXPECT_EQ(game::common::edge_request_id(*response), request_id);
    const auto accepted = game::common::decode_edge_attach_accepted(*response);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->account_id(), 42U);
}

/// #75 保留拒绝映射:身份凭据与 Queue Number 分属不同错误码,
/// 但都用 1999 回包并回显请求 ID。#79 后续会替换准入凭据
/// 类型,不得在 #75 偷改现有线行为。
TEST_F(ServiceFrameEdgeBudgetTest, AttachRejectionsPreserveWireContract) {
    constexpr std::uint64_t identity_request_id = 41;
    const auto invalid_identity = attach_with_tokens(
        "not-an-identity-token", "not-a-number", identity_request_id);
    const auto identity_response =
        receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(identity_response.has_value());
    EXPECT_EQ(
        game::common::edge_message_id(*identity_response),
        game::common::EdgeMessageId::MESSAGE_ID_S2C_ERROR);
    EXPECT_EQ(
        game::common::edge_request_id(*identity_response), identity_request_id);
    const auto identity_error =
        game::common::decode_edge_error(*identity_response);
    ASSERT_TRUE(identity_error.has_value());
    EXPECT_EQ(
        identity_error->code(), game::common::edge_error_invalid_credentials);

    constexpr std::uint64_t number_request_id = 43;
    const auto invalid_number =
        attach("aaaa000000000014aaaa000000000014", false, number_request_id);
    const auto number_response = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(number_response.has_value());
    EXPECT_EQ(
        game::common::edge_message_id(*number_response),
        game::common::EdgeMessageId::MESSAGE_ID_S2C_ERROR);
    EXPECT_EQ(
        game::common::edge_request_id(*number_response), number_request_id);
    const auto number_error = game::common::decode_edge_error(*number_response);
    ASSERT_TRUE(number_error.has_value());
    EXPECT_EQ(
        number_error->code(), game::common::edge_error_invalid_queue_number);
    const auto metrics = metrics_.render();
    EXPECT_NE(
        metrics.find("# TYPE edge_jti_replay_rejected_total counter\n"),
        std::string::npos);
    EXPECT_NE(
        metrics.find("edge_jti_replay_rejected_total 0\n"), std::string::npos);
}

// #79 replacement point: rejection diagnostics may expose the failure class,
// but not bearer material or identity correlation values.
TEST_F(
    ServiceFrameEdgeBudgetTest,
    CredentialFailuresKeepRawCredentialsAndJtiOutOfTelemetry) {
    const std::string raw_identity = "raw-secret-identity-token";
    const std::string raw_number = "raw-secret-queue-number-token";
    const std::string identity_jti = "aaaa000000000079aaaa000000000079";

    const auto invalid_identity_client =
        attach_with_tokens(raw_identity, raw_number, 79);
    ASSERT_TRUE(receive_while_driving(std::chrono::seconds{2}).has_value());

    const auto now = std::chrono::system_clock::now();
    const auto valid_identity = identity_codec_->issue(
        game::common::IdentityClaims{
            .issuer = "realmmesh/login-verify",
            .account_id = 42,
            .jti = identity_jti,
            .issued_at = now,
            .expires_at = now + std::chrono::minutes{30}});
    const auto invalid_number_client =
        attach_with_tokens(valid_identity, raw_number, 80);
    ASSERT_TRUE(receive_while_driving(std::chrono::seconds{2}).has_value());

    ASSERT_TRUE(logger_->flush(std::chrono::seconds{2}));
    std::ifstream log_stream(log_directory_->path() / "frame.log");
    const std::string logs{
        std::istreambuf_iterator<char>{log_stream},
        std::istreambuf_iterator<char>{}};
    const auto metrics = metrics_.render();
    for (const auto& secret :
         {raw_identity, raw_number, valid_identity, identity_jti}) {
        EXPECT_EQ(logs.find(secret), std::string::npos);
        EXPECT_EQ(metrics.find(secret), std::string::npos);
    }
}

TEST_F(ServiceFrameEdgeBudgetTest, ReplayedJtiPreservesWireRejection) {
    const auto first = attach("aaaa000000000002aaaa000000000002");
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->fetch_free == 1;
        },
        std::chrono::seconds{2}));

    // 同一身份令牌(jti)在新连接重放:凭据无效 → 拒绝并终结,拉取额度
    // 不被消耗(conn 的一次瞬时占用由 SessionClosed 归还)。
    constexpr std::uint64_t request_id = 53;
    const auto replayed =
        attach("aaaa000000000002aaaa000000000002", true, request_id);
    const auto response = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(
        game::common::edge_message_id(*response),
        game::common::EdgeMessageId::MESSAGE_ID_S2C_ERROR);
    EXPECT_EQ(game::common::edge_request_id(*response), request_id);
    const auto error = game::common::decode_edge_error(*response);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code(), game::common::edge_error_invalid_credentials);
    // 错误帧到达时关闭可能已结算,不固化瞬时 conn_free=2。
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 3;
        },
        std::chrono::seconds{2}));
    EXPECT_EQ(*observed_budget(), InstanceBudgetSnapshot({3, 1, true}));
}

/// 已退役编号(#50):1101 曾在旧链承载 Realm 认证;未 attach 的会话发它
/// (或任何非 1301 消息)一律未认证拒绝并终结,不当业务消息处理。
TEST_F(ServiceFrameEdgeBudgetTest, RetiredMessageIdIsRefused) {
    auto client = std::make_unique<AttachClient>(
        static_cast<std::uint16_t>(runtime_->local_endpoints().front().port));
    client_ = client.get();
    const network::LengthFieldCodec codec(1024);
    client->send(codec.encode(test_support::edge_raw_frame(1101, 6)));

    const auto response = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(response.has_value());
    const auto error = game::common::decode_edge_error(*response);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(
        game::common::edge_message_id(*response),
        game::common::EdgeMessageId::MESSAGE_ID_S2C_ERROR);
    // 已退役编号不通过公共 Envelope 解码,Gateway 因而使用
    // request_id=0 的兼容回退;合法 1301 的回显契约由 attach
    // 场景锁定。
    EXPECT_EQ(game::common::edge_request_id(*response), 0U);
    EXPECT_EQ(error->code(), game::common::edge_error_not_authenticated);
    EXPECT_TRUE(client->saw_close(std::chrono::seconds{2}));
}

TEST_F(
    ServiceFrameEdgeBudgetTest, OutOfBudgetAttachIsRejectedWithoutConsuming) {
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
    constexpr std::uint64_t request_id = 47;
    const auto third =
        attach("aaaa000000000005aaaa000000000005", true, request_id);
    const auto response = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(
        game::common::edge_message_id(*response),
        game::common::EdgeMessageId::MESSAGE_ID_S2C_ERROR);
    EXPECT_EQ(game::common::edge_request_id(*response), request_id);
    const auto error = game::common::decode_edge_error(*response);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code(), game::common::edge_error_attach_out_of_budget);
    // 读到拒绝帧时 SessionClosed 可能已随同一 IO 周期到达;
    // 只冻结对外稳定的最终预算,不把瞬时 conn_free=1 的调度
    // 时隙当成契约。
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 2 &&
                   budget->fetch_free == 0;
        },
        std::chrono::seconds{2}));
    EXPECT_EQ(*observed_budget(), InstanceBudgetSnapshot({2, 0, true}));
}

TEST_F(
    ServiceFrameEdgeBudgetTest,
    StoppedFetchPublishesUnavailableBudgetAndStopsGateway) {
    fetch_port_->stop();
    const auto client = attach("aaaa000000000017aaaa000000000017");

    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 0 &&
                   budget->fetch_free == 0;
        },
        std::chrono::seconds{2}));
    EXPECT_FALSE(runtime_->running());
}

/// 拉取成功(#44):fetching → handed-off,拉取槽即还,conn 保持占用
/// (#45 起签发票据进入宽限,宽限内会话仍在管,收尾行为见 #45 用例)。
/// #75 不在旧 ServiceFrame seam 固化「fetch 先于已到达 close」
/// 的错误次序;#77 必须在新 Pipeline seam 另加 close-wins 的
/// 确定性场景。
TEST_F(ServiceFrameEdgeBudgetTest, FetchSuccessTransitionsToHandedOff) {
    register_realm_endpoint();
    rebuild_frame(true, std::chrono::milliseconds{0}, std::chrono::seconds{2});

    const auto client = attach("aaaa000000000006aaaa000000000006");
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 3 &&
                   budget->fetch_free == 2;
        },
        std::chrono::seconds{2},
        &*realm_resolver_));
    EXPECT_EQ(*observed_budget(), InstanceBudgetSnapshot({3, 2, true}));
}

/// handoff 签发(#45):拉取成功即推送 EnterRealmGranted,票据可按
/// EnterRealm 用途兑换出账号 claims,端点指向发现到的 realm;宽限
/// 到期未迁移由管线关闭,conn 预算归还。使用最小合法 1ms 宽限，
/// 保持真实配置校验与生产时序一致；精确边界由 Pipeline 单测的
/// GatewayLoginFrame.now 覆盖。
TEST_F(ServiceFrameEdgeBudgetTest, GrantIssuedThenGraceCloses) {
    register_realm_endpoint();
    rebuild_frame(
        true,
        std::chrono::milliseconds{0},
        std::chrono::seconds{2},
        std::chrono::milliseconds{1});

    const auto client = attach("aaaa000000000008aaaa000000000008");

    // 1302 attach accepted 在前,1303 grant 在后(服务器主动推送,
    // request_id 恒 0);两帧都经「边推帧边收」取得,不受宽限影响。
    const auto accepted_payload =
        receive_while_driving(std::chrono::seconds{2}, &*realm_resolver_);
    ASSERT_TRUE(accepted_payload.has_value());
    const auto accepted =
        game::common::decode_edge_attach_accepted(*accepted_payload);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(
        game::common::edge_message_id(*accepted_payload),
        game::common::EdgeMessageId::MESSAGE_ID_S2C_EDGE_ATTACH_ACCEPTED);
    EXPECT_EQ(game::common::edge_request_id(*accepted_payload), 5U);
    EXPECT_EQ(accepted->account_id(), 42U);

    const auto granted_payload =
        receive_while_driving(std::chrono::seconds{2}, &*realm_resolver_);
    ASSERT_TRUE(granted_payload.has_value());
    const auto granted =
        game::common::decode_enter_realm_granted(*granted_payload);
    ASSERT_TRUE(granted.has_value());
    EXPECT_EQ(
        game::common::edge_message_id(*granted_payload),
        game::common::EdgeMessageId::MESSAGE_ID_S2C_ENTER_REALM_GRANTED);
    EXPECT_EQ(game::common::edge_request_id(*granted_payload), 0U);
    ASSERT_EQ(granted->realm_endpoints_size(), 1);
    EXPECT_EQ(granted->realm_endpoints(0).address(), "127.0.0.1");
    EXPECT_EQ(granted->realm_endpoints(0).port(), 7100);
    EXPECT_EQ(
        granted->realm_endpoints(0).protocol(),
        ::realmmesh::protocol::edge::v1::TRANSPORT_PROTOCOL_TLS_TCP);
    EXPECT_EQ(granted->realm_endpoints(0).priority(), 0U);
    game::common::SessionTickets verifier(
        game::common::parse_ticket_key_hex(
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
    ASSERT_TRUE(observed_budget().has_value());
    EXPECT_EQ(*observed_budget(), InstanceBudgetSnapshot({3, 2, true}));

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

/// 真实 Primary Transport 的 SessionClosed 必须流经 ServiceFrame,
/// 从 fetching 同时归还 conn 与 fetch 两份 Admission Budget。
TEST_F(ServiceFrameEdgeBudgetTest, RealPeerCloseReturnsBothBudgets) {
    auto client = attach("aaaa000000000016aaaa000000000016");
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 3 &&
                   budget->fetch_free == 1;
        },
        std::chrono::seconds{2}));

    client.reset();
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 4 &&
                   budget->fetch_free == 2;
        },
        std::chrono::seconds{2}));
    EXPECT_EQ(*observed_budget(), InstanceBudgetSnapshot({4, 2, true}));
}

/// 降级路径(#45):发现缺失且未配置静态下游 → 不签发票据,告警后
/// 直接断开,双预算归还。
TEST_F(ServiceFrameEdgeBudgetTest, NoEndpointClosesWithoutGrant) {
    rebuild_frame(true, std::chrono::milliseconds{0}, std::chrono::seconds{2});

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
    rebuild_frame(
        true,
        std::chrono::milliseconds{0},
        std::chrono::seconds{2},
        std::chrono::seconds{5},
        "127.0.0.1",
        7100);

    const auto client = attach("aaaa000000000010aaaa000000000010");
    // 1302 在前,1303 在后;本用例只关心 1303 的端点内容。
    ASSERT_TRUE(receive_while_driving(std::chrono::seconds{2}).has_value());
    const auto granted_payload = receive_while_driving(std::chrono::seconds{2});
    ASSERT_TRUE(granted_payload.has_value());
    const auto granted =
        game::common::decode_enter_realm_granted(*granted_payload);
    ASSERT_TRUE(granted.has_value());
    ASSERT_EQ(granted->realm_endpoints_size(), 1);
    EXPECT_EQ(granted->realm_endpoints(0).address(), "127.0.0.1");
    EXPECT_EQ(granted->realm_endpoints(0).port(), 7100);
    EXPECT_EQ(granted->realm_endpoints(0).priority(), 0U);

    // 签发后进入宽限:conn 保持占用,不降级断开。
    ASSERT_TRUE(drive_until(
        [](const std::optional<InstanceBudgetSnapshot>& budget) {
            return budget.has_value() && budget->conn_free == 3 &&
                   budget->fetch_free == 2;
        },
        std::chrono::seconds{2}));
    EXPECT_EQ(*observed_budget(), InstanceBudgetSnapshot({3, 2, true}));
}

/// #75 保留端点优先级:同一帧同时有动态发现与静态兜底时,
/// 动态 Realm 端点胜出;静态值只在发现缺失时使用。
TEST_F(
    ServiceFrameEdgeBudgetTest, DiscoveredRealmEndpointWinsOverStaticFallback) {
    register_realm_endpoint("127.0.0.2", 7101);
    rebuild_frame(
        true,
        std::chrono::milliseconds{0},
        std::chrono::seconds{2},
        std::chrono::seconds{5},
        "192.0.2.10",
        7200);

    const auto client = attach("aaaa000000000015aaaa000000000015");
    ASSERT_TRUE(
        receive_while_driving(std::chrono::seconds{2}, &*realm_resolver_)
            .has_value());
    const auto granted_payload =
        receive_while_driving(std::chrono::seconds{2}, &*realm_resolver_);
    ASSERT_TRUE(granted_payload.has_value());
    const auto granted =
        game::common::decode_enter_realm_granted(*granted_payload);
    ASSERT_TRUE(granted.has_value());
    ASSERT_EQ(granted->realm_endpoints_size(), 1);
    EXPECT_EQ(granted->realm_endpoints(0).address(), "127.0.0.2");
    EXPECT_EQ(granted->realm_endpoints(0).port(), 7101);
    EXPECT_EQ(granted->realm_endpoints(0).priority(), 0U);
}

/// 重试耗尽(#44):基数 30ms 连挂 4 次(首发 + 3 次重试)后上报耗尽,
/// 调用方断开会话 —— conn 与 fetch 双预算随 #43 关闭路径归还。
TEST_F(ServiceFrameEdgeBudgetTest, ExhaustedFetchClosesAndReturnsBudgets) {
    rebuild_frame(
        false, std::chrono::milliseconds{0}, std::chrono::milliseconds{30});

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
    EXPECT_EQ(*observed_budget(), InstanceBudgetSnapshot({4, 2, true}));
    // 失败/耗尽不属于成功拉取耗时样本。
    EXPECT_EQ(
        metrics_.render().find("edge_fetch_duration_seconds_count"),
        std::string::npos);
}

/// 帧尾指标发布(#47):慢拉取源下会话可见 fetching 水位与 fetch_free
/// 消耗;成功交付后迁 handed-off,拉取耗时进直方图,重试计数为 0。
TEST_F(ServiceFrameEdgeBudgetTest, MetricsFollowPipelineStagesAndFetch) {
    register_realm_endpoint();
    rebuild_frame(
        true, std::chrono::milliseconds{600}, std::chrono::seconds{2});

    // attach 前:空管线,三阶段 gauge 全 0,双预算满 {4,2}。
    frame_->tick(*logger_, *runtime_, nullptr, &*reporter_);
    const auto idle = metrics_.render();
    EXPECT_NE(idle.find("# TYPE edge_sessions gauge\n"), std::string::npos);
    EXPECT_NE(idle.find("# TYPE edge_budget gauge\n"), std::string::npos);
    EXPECT_NE(
        idle.find("# TYPE edge_fetch_retry_total counter\n"),
        std::string::npos);
    EXPECT_NE(
        idle.find("edge_sessions{stage=\"pending\"} 0\n"), std::string::npos);
    EXPECT_NE(
        idle.find("edge_sessions{stage=\"fetching\"} 0\n"), std::string::npos);
    EXPECT_NE(
        idle.find("edge_sessions{stage=\"handed_off\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        idle.find("edge_budget{kind=\"conn_free\"} 4\n"), std::string::npos);
    EXPECT_NE(
        idle.find("edge_budget{kind=\"fetch_free\"} 2\n"), std::string::npos);
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
    EXPECT_NE(
        delivered.find("# TYPE edge_fetch_duration_seconds histogram\n"),
        std::string::npos);
    EXPECT_NE(
        delivered.find("# TYPE edge_jti_replay_rejected_total counter\n"),
        std::string::npos);
    EXPECT_NE(
        delivered.find("edge_sessions{stage=\"fetching\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        delivered.find("edge_budget{kind=\"fetch_free\"} 2\n"),
        std::string::npos);
    EXPECT_NE(
        delivered.find("edge_fetch_duration_seconds_bucket{le=\"1\"} 1\n"),
        std::string::npos);
    EXPECT_NE(
        delivered.find("edge_fetch_duration_seconds_sum 0.6\n"),
        std::string::npos);
}

/// 重试与重放计数(#47):30ms 基数耗尽(3 次重试)计入
/// edge_fetch_retry_total;同 jti 新连接重放被拒后计入
/// edge_jti_replay_rejected_total。
TEST_F(ServiceFrameEdgeBudgetTest, MetricsCountRetriesAndReplayRejections) {
    rebuild_frame(
        false, std::chrono::milliseconds{0}, std::chrono::milliseconds{30});

    const auto first = attach("aaaa000000000012aaaa000000000012");
    ASSERT_TRUE(drive_until_metrics(
        [](const std::string& text) {
            return text.find("edge_fetch_retry_total 3\n") != std::string::npos;
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
