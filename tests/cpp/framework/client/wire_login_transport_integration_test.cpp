/// #49 集成档:真实 TLS loopback(临时端口)+ 生产 WireLoginTransport +
/// LoginChain 驱动,断言两段竞速真的建上连、HTTP 段按契约携带凭据。
///
/// 为什么用桩而不是 MeshHost:Realm 段的 EnterRealm 兑换协议归 #46
/// (edge.proto 只有 1303 下行票据),业务服尚不存在。网关桩按真实
/// 1301/1302/1303 帧交互、Realm 桩按真实 TLS 字节流交互,兑换内容由测试
/// 注入;#46 落地后把桩换成真服务即可,链路代码不动。
///
/// 帧约定说明:TlsTcpTransport 自带 4 字节长度前缀(message 语义:
/// MessageReceived/ send() 收发的都是去帧的消息体),所以桩里直接按
/// 信封字节处理,不需要也不允许再套一层长度前缀。

#include "realmmesh/client/wire_login_transport.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/network/client/edge_client_connection.hpp"
#include "realmmesh/network/http/http_server.hpp"
#include "realmmesh/network/transport/transport_factory.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace realm::client {
namespace {

namespace common = ::realm::game::common;
namespace edge_v1 = ::realmmesh::protocol::edge::v1;
namespace net_client = ::realm::network::client;

using std::chrono::milliseconds;

[[nodiscard]] network::Http1Response json_response(int status,
                                                  std::string body) {
    network::Http1Response response;
    response.status = status;
    response.headers.emplace_back("Content-Type", "application/json");
    response.body = std::move(body);
    return response;
}

/// 登录健全服 + 排队调度服的桩:真实 HTTPS(ALPN http/1.1),按服务契约
/// 回 JSON;tickets/me 首次回 401 + 2001 以触发客户端自动重取。
class HttpStub final {
public:
    struct Observed final {
        std::string method;
        std::string target;
        std::string authorization;
        std::string body;
    };

    HttpStub()
        : server_("127.0.0.1", 0, stub_config(),
                  [this](const network::Http1Request& request) {
                      return handle(request);
                  }) {}

    [[nodiscard]] std::uint16_t port() const noexcept {
        return server_.local_port();
    }

    void poll(milliseconds timeout) { server_.poll_once(timeout); }

    /// tickets/me 前 N 次回 401 + 2001(号牌过期)。
    std::atomic<int> unauthorized_remaining{1};
    /// 注入的放行宽限(tickets/me 的 expires_in)。
    int admit_grace_seconds{300};

    [[nodiscard]] int count_of(std::string_view target) const {
        std::lock_guard lock(mutex_);
        return static_cast<int>(
            std::count_if(observed_.begin(), observed_.end(),
                          [target](const Observed& entry) {
                              return entry.target == target;
                          }));
    }

    [[nodiscard]] std::vector<Observed> observed() const {
        std::lock_guard lock(mutex_);
        return observed_;
    }

private:
    [[nodiscard]] static network::HttpServerConfig stub_config() {
        network::HttpServerConfig config;
        config.tls_identity.certificate_chain_file =
            REALMMESH_TEST_TLS_CERTIFICATE;
        config.tls_identity.private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY;
        config.tls_identity.alpn = "http/1.1";
        return config;
    }

    [[nodiscard]] network::Http1Response handle(
        const network::Http1Request& request) {
        const std::string* authorization = request.header("authorization");
        {
            std::lock_guard lock(mutex_);
            observed_.push_back(Observed{
                request.method,
                request.target,
                authorization == nullptr ? std::string{} : *authorization,
                request.body,
            });
        }

        if (request.method == "POST" && request.target == "/v1/login/verify") {
            return json_response(
                200,
                R"({"identity_token":"identity-1","account_id":"42",)"
                R"("expires_in":600})");
        }
        if (request.method == "POST" && request.target == "/v1/queue/tickets") {
            const auto issued = ++tickets_issued_;
            return json_response(
                202,
                "{\"queue_number_token\":\"number-token-" +
                    std::to_string(issued) +
                    "\",\"number\":100,\"estimated_wait_seconds\":1}");
        }
        if (request.method == "GET" && request.target == "/v1/queue/progress") {
            // 首个 progress 报 0,之后报 100:保证链路真的等满一个轮询间隔
            // (分档节奏端到端接上),再进放行判定。
            const auto calls = ++progress_calls_;
            const int released = calls >= 2 ? 100 : 0;
            return json_response(
                200,
                "{\"released_number\":" + std::to_string(released) +
                    ",\"admit_rate\":100,\"server_time\":0}");
        }
        if (request.method == "GET" &&
            request.target == "/v1/queue/tickets/me") {
            ++me_calls_;
            if (unauthorized_remaining.load() > 0) {
                --unauthorized_remaining;
                return json_response(
                    401, R"({"code":2001,"message":"number token expired"})");
            }
            return json_response(
                200,
                "{\"admit_grant\":{\"queue_number_token\":\"grant-1\","
                "\"number\":100,\"expires_in\":" +
                    std::to_string(admit_grace_seconds) +
                    "},\"status\":\"admitted\",\"position\":0,"
                    "\"estimated_wait_seconds\":0}");
        }
        return json_response(404, R"({"code":1404,"message":"unknown route"})");
    }

    mutable std::mutex mutex_;
    std::vector<Observed> observed_;
    std::atomic<int> tickets_issued_{0};
    std::atomic<int> progress_calls_{0};
    std::atomic<int> me_calls_{0};
    network::HttpServer server_;
};

/// 网关桩:真实 TLS(ALPN realmmesh-edge/1)。收到 1301 后回 1302,随即推送
/// 1303(票据 + realm 端点:QUIC 主 + TLS/TCP 降级)。
class EdgeStub final {
public:
    EdgeStub(network::IMessageTransport& transport, std::uint16_t realm_port)
        : transport_(transport), realm_port_(realm_port) {}

    void poll(milliseconds timeout) {
        for (const auto& event : transport_.poll_once(timeout)) {
            if (event.kind == network::TransportEventKind::MessageReceived) {
                handle_frame(event.session_id, event.payload);
            }
        }
    }

    /// 前 N 次 attach 回 1999 + 2001(号牌过期)。
    std::atomic<int> reject_attach_remaining{0};
    std::atomic<int> attach_calls{0};
    std::atomic<int> handoff_sent{0};

    [[nodiscard]] std::string last_identity_token() const {
        std::lock_guard lock(mutex_);
        return last_identity_token_;
    }
    [[nodiscard]] std::string last_number_token() const {
        std::lock_guard lock(mutex_);
        return last_number_token_;
    }

private:
    void handle_frame(network::SessionId session_id,
                      std::span<const std::byte> payload) {
        const auto message_id = common::edge_message_id(payload);
        if (!message_id.has_value() ||
            *message_id != edge_v1::MESSAGE_ID_C2S_EDGE_ATTACH) {
            return;
        }
        attach_calls.fetch_add(1);
        const auto attach = common::decode_edge_attach(payload);
        {
            std::lock_guard lock(mutex_);
            last_identity_token_ =
                attach.has_value() ? attach->identity_token() : std::string{};
            last_number_token_ = attach.has_value()
                                     ? attach->queue_number_token()
                                     : std::string{};
        }

        if (reject_attach_remaining.load() > 0) {
            reject_attach_remaining.fetch_sub(1);
            common::EdgeError error;
            error.set_code(static_cast<std::uint32_t>(
                common::edge_error_invalid_queue_number));
            error.set_message("number token expired");
            send(session_id, common::encode(error));
            return;
        }

        common::EdgeAttachAccepted accepted;
        accepted.set_account_id(42);
        send(session_id, common::encode(accepted));

        common::EnterRealmGranted granted;
        granted.set_enter_realm_ticket("ert-1");
        auto* quic = granted.add_realm_endpoints();
        quic->set_address("127.0.0.1");
        quic->set_port(realm_port_);
        quic->set_protocol(edge_v1::TRANSPORT_PROTOCOL_QUIC);
        quic->set_priority(1);
        auto* tls_tcp = granted.add_realm_endpoints();
        tls_tcp->set_address("127.0.0.1");
        tls_tcp->set_port(realm_port_);
        tls_tcp->set_protocol(edge_v1::TRANSPORT_PROTOCOL_TLS_TCP);
        tls_tcp->set_priority(0);
        send(session_id, common::encode(granted));
        handoff_sent.fetch_add(1);
    }

    void send(network::SessionId session_id,
              std::span<const std::byte> payload) {
        // 桩端不做背压:发送失败即测试环境坏,交给后续断言暴露。
        static_cast<void>(transport_.send(session_id, payload));
    }

    network::IMessageTransport& transport_;
    std::uint16_t realm_port_{0};
    mutable std::mutex mutex_;
    std::string last_identity_token_;
    std::string last_number_token_;
};

/// 业务服桩:真实 TLS(ALPN realmmesh-realm/1)。收到兑换字节即回执;
/// 兑换内容归 #46(桩只证明字节真的到了业务服),帧约定沿用 mesh 服务
/// 的 message 语义。
class RealmStub final {
public:
    explicit RealmStub(network::IMessageTransport& transport)
        : transport_(transport) {}

    void poll(milliseconds timeout) {
        for (const auto& event : transport_.poll_once(timeout)) {
            if (event.kind != network::TransportEventKind::MessageReceived) {
                continue;
            }
            std::lock_guard lock(mutex_);
            received_.append(
                reinterpret_cast<const char*>(event.payload.data()),
                event.payload.size());
            if (!replied_) {
                replied_ = true;
                const std::string ack{"WELCOME"};
                static_cast<void>(transport_.send(
                    event.session_id,
                    std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(ack.data()),
                        ack.size())));
            }
        }
    }

    [[nodiscard]] std::string received() const {
        std::lock_guard lock(mutex_);
        return received_;
    }

private:
    network::IMessageTransport& transport_;
    mutable std::mutex mutex_;
    std::string received_;
    bool replied_{false};
};

/// 兑换口实现:在已直连的 Realm 流上按 mesh 帧约定写票据、读回执。
/// 复用生产的 EdgeClientConnection(它就是「长度前缀帧 + 收发一帧」),
/// 别名共享指针只是借用调用方的流,不接管生命周期。
class StubRedeemer final : public EnterRealmRedeemer {
public:
    [[nodiscard]] PortStatus redeem(net_client::ISecureByteStream& stream,
                                    std::string_view enter_realm_ticket,
                                    TimePoint deadline) override {
        last_ticket = std::string{enter_realm_ticket};
        net_client::EdgeClientConnection connection{
            std::shared_ptr<net_client::ISecureByteStream>(&stream,
                                                           [](auto*) {})};
        if (!connection.send_frame(common::protobuf_bytes(enter_realm_ticket),
                                   deadline)) {
            return PortStatus::error(ChainFailure::EnterRealmRejected,
                                     "写票据失败");
        }
        const auto reply = connection.receive_frame(deadline);
        if (!reply.has_value()) {
            return PortStatus::error(ChainFailure::EnterRealmRejected,
                                     "读回执失败");
        }
        const std::string text(reinterpret_cast<const char*>(reply->data()),
                               reply->size());
        if (text != "WELCOME") {
            return PortStatus::error(ChainFailure::EnterRealmRejected,
                                     "回执不符: " + text);
        }
        return PortStatus::success();
    }

    std::string last_ticket;
};

/// 三个桩共用一个驱动线程:HttpServer 与两条消息传输都是轮询模型
/// (属主线程驱动 poll_once),与生产服务同形。
class PollDriver final {
public:
    PollDriver(HttpStub& http, EdgeStub& edge, RealmStub& realm)
        : thread_([&http, &edge, &realm](std::stop_token stop) {
              while (!stop.stop_requested()) {
                  http.poll(milliseconds{1});
                  edge.poll(milliseconds{1});
                  realm.poll(milliseconds{1});
              }
          }) {}

private:
    std::jthread thread_;
};

/// 网关桩 + 业务服桩的传输配置(真实 TLS,ALPN 各自就位)。
[[nodiscard]] std::array<network::TransportConfig, 2> stub_configs() {
    auto gateway = network::TransportConfig{
        .name = "gateway-stub",
        .protocol = network::TransportProtocol::TlsTcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
    };
    gateway.tls = network::TransportConfig::TlsServerIdentity{
        .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
        .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
        .alpn = "realmmesh-edge/1",
    };
    auto realm = network::TransportConfig{
        .name = "realm-stub",
        .protocol = network::TransportProtocol::TlsTcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
    };
    realm.tls = network::TransportConfig::TlsServerIdentity{
        .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
        .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
        .alpn = "realmmesh-realm/1",
    };
    return {std::move(gateway), std::move(realm)};
}

/// 生产链路压真实 TLS 端口的旋钮:压缩轮询/重试时标(结构不变,规格数值
/// 由 adaptive_poller_test 钉死),竞速的 TLS/TCP 起跑延迟压到 20ms。
[[nodiscard]] LoginChainConfig fast_chain_config(std::uint16_t gateway_port) {
    LoginChainConfig config;
    config.poll.initial_interval = milliseconds{20};
    config.poll.far_interval = milliseconds{20};
    config.poll.near_interval = milliseconds{10};
    config.poll.min_interval = milliseconds{0};
    config.poll.backoff_cap = milliseconds{40};
    config.poll.jitter_ratio = 0.0;
    config.gateway_retry_delay = milliseconds{20};
    config.realm_retry_delay = milliseconds{20};
    // QUIC 候选在前:本仓无 QUIC 客户端(ADR-0002)→ dialog 立刻判
    // Unsupported → 竞速按 permits_transport_fallback 转 TLS/TCP。
    config.gateway_endpoints.push_back(net_client::EndpointCandidate{
        .protocol = network::TransportProtocol::Quic,
        .host = "127.0.0.1",
        .port = gateway_port,
        .priority = 0,
    });
    config.gateway_endpoints.push_back(net_client::EndpointCandidate{
        .protocol = network::TransportProtocol::TlsTcp,
        .host = "127.0.0.1",
        .port = gateway_port,
        .priority = 1,
    });
    return config;
}

[[nodiscard]] WireTransportOptions fast_wire_options() {
    WireTransportOptions options;
    options.request_timeout = std::chrono::seconds{5};
    options.connector.tls_tcp_delay = milliseconds{20};
    return options;
}

/// 七态全走真实 TLS:HTTP 段(verify/取号/轮询/查号)按契约带凭据,
/// 网关段用 attach 帧拿到 1303,Realm 段再起一次竞速并把票据写到业务服。
TEST(WireLoginTransportIntegrationTest, DrivesLoginChainOverRealTls) {
    const auto configs = stub_configs();
    auto transports = network::TransportFactory::create_enabled(configs);
    ASSERT_EQ(transports.size(), 2U);
    const auto gateway_port = transports.at(0)->local_endpoint().port;
    const auto realm_port = transports.at(1)->local_endpoint().port;
    ASSERT_NE(gateway_port, 0);
    ASSERT_NE(realm_port, 0);

    HttpStub http;
    EdgeStub edge(*transports.at(0), realm_port);
    RealmStub realm(*transports.at(1));
    const PollDriver driver(http, edge, realm);

    WireEndpoints endpoints;
    endpoints.login_verify_port = http.port();
    endpoints.queue_port = http.port();
    endpoints.realm_alpn = "realmmesh-realm/1";
    // 测试证书自签:显式关闭校验(生产默认校验)。
    endpoints.verify_peer = false;

    StubRedeemer redeemer;
    WireLoginTransport transport(endpoints, redeemer, fast_wire_options());

    LoginChain chain(transport, fast_chain_config(gateway_port));
    const auto result =
        chain.run("alice", "secret", Clock::now() + std::chrono::seconds{10});

    ASSERT_TRUE(result.succeeded())
        << "failure=" << chain_failure_name(result.failure)
        << " detail=" << result.detail;
    EXPECT_EQ(result.stage, LoginStage::InGame);
    EXPECT_EQ(result.number, 100U);

    // HTTP 段:verify 一次;progress 三次(首次未放行,第二次放行但号牌
    // 被判过期,重取后第三次放行);tickets 两次;查号两次。
    EXPECT_EQ(http.count_of("/v1/login/verify"), 1);
    EXPECT_EQ(http.count_of("/v1/queue/tickets"), 2);
    EXPECT_EQ(http.count_of("/v1/queue/progress"), 3);
    EXPECT_EQ(http.count_of("/v1/queue/tickets/me"), 2);

    // 凭据纯内存传递:取号带身份 token,查号带当期号牌。
    const auto observed = http.observed();
    for (const auto& entry : observed) {
        if (entry.target == "/v1/queue/tickets") {
            EXPECT_EQ(entry.authorization, "Bearer identity-1");
        } else if (entry.target == "/v1/queue/tickets/me") {
            EXPECT_TRUE(entry.authorization == "Bearer number-token-1" ||
                        entry.authorization == "Bearer number-token-2")
                << entry.authorization;
        } else if (entry.target == "/v1/login/verify") {
            EXPECT_EQ(entry.body,
                      R"({"account":"alice","credential":"secret"})");
        }
    }

    // 网关段:QUIC 候选即时被否,TLS/TCP 真的承载了 attach 帧。
    EXPECT_EQ(edge.attach_calls.load(), 1);
    EXPECT_EQ(edge.last_identity_token(), "identity-1");
    // admit 后用重签号牌 attach,不是原始号牌。
    EXPECT_EQ(edge.last_number_token(), "grant-1");
    EXPECT_EQ(edge.handoff_sent.load(), 1);

    // Realm 段:第二次竞速真的把 1303 的票据写到了业务服桩上。
    EXPECT_EQ(redeemer.last_ticket, "ert-1");
    EXPECT_EQ(realm.received(), "ert-1");
    EXPECT_EQ(chain.credentials().enter_realm_ticket, "ert-1");
}

/// attach 被 1999 + 2001 拒(号牌过期):真实帧路径上同样自动重取号牌,
/// 不需要人类重新登录(spec §7 回退规则)。
TEST(WireLoginTransportIntegrationTest, AttachRejectionRetakesNumberToken) {
    const auto configs = stub_configs();
    auto transports = network::TransportFactory::create_enabled(configs);
    ASSERT_EQ(transports.size(), 2U);
    const auto gateway_port = transports.at(0)->local_endpoint().port;
    const auto realm_port = transports.at(1)->local_endpoint().port;

    HttpStub http;
    http.unauthorized_remaining.store(0);
    EdgeStub edge(*transports.at(0), realm_port);
    edge.reject_attach_remaining.store(1);
    RealmStub realm(*transports.at(1));
    const PollDriver driver(http, edge, realm);

    WireEndpoints endpoints;
    endpoints.login_verify_port = http.port();
    endpoints.queue_port = http.port();
    endpoints.realm_alpn = "realmmesh-realm/1";
    endpoints.verify_peer = false;

    StubRedeemer redeemer;
    WireLoginTransport transport(endpoints, redeemer, fast_wire_options());

    LoginChain chain(transport, fast_chain_config(gateway_port));
    const auto result =
        chain.run("alice", "secret", Clock::now() + std::chrono::seconds{10});

    ASSERT_TRUE(result.succeeded())
        << "failure=" << chain_failure_name(result.failure)
        << " detail=" << result.detail;
    // 两次 attach 都是真实帧:第一次被 2001 拒,第二次放行并交付。
    EXPECT_EQ(edge.attach_calls.load(), 2);
    EXPECT_EQ(edge.handoff_sent.load(), 1);
    EXPECT_EQ(http.count_of("/v1/queue/tickets"), 2);
    EXPECT_EQ(chain.credentials().number, 100U);
    EXPECT_EQ(realm.received(), "ert-1");
}

/// #46 落地前的默认兑换口必须明确判失败(不得猜协议):不写字节、不回执。
/// 链路侧的 Realm 段回退(窗口内重试、回网关重入)由 login_chain_test 覆盖。
TEST(PendingEnterRealmRedeemerTest, FailsExplicitlyUntilProtocolLands) {
    class CountingStream final : public net_client::ISecureByteStream {
    public:
        [[nodiscard]] bool write_all(std::span<const std::byte> data,
                                     net_client::StreamDeadline deadline)
            override {
            static_cast<void>(deadline);
            writes += static_cast<int>(data.size());
            return true;
        }
        [[nodiscard]] std::optional<std::size_t> read_some(
            std::span<std::byte> out,
            net_client::StreamDeadline deadline) override {
            static_cast<void>(out);
            static_cast<void>(deadline);
            return std::nullopt;
        }
        void shutdown() override {}

        int writes{0};
    };

    CountingStream stream;
    PendingEnterRealmRedeemer redeemer;
    const auto status =
        redeemer.redeem(stream, "ert-1", Clock::now() + std::chrono::seconds{1});

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.failure, ChainFailure::EnterRealmRejected);
    EXPECT_NE(status.detail.find("#46"), std::string::npos) << status.detail;
    EXPECT_EQ(stream.writes, 0);
}

}  // namespace
}  // namespace realm::client
