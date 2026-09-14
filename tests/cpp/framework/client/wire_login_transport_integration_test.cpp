/// #49 集成档:真实 TLS loopback(临时端口)+ 生产 WireLoginTransport +
/// LoginChain 驱动,断言两段竞速真的建上连、HTTP 段按契约携带凭据、Realm
/// 段在生产兑换口上完成 1304/1305 兑换。
///
/// 两端的「真实」程度:#49 时 Realm 侧兑换协议(#46)未落地,故用桩注入
/// 兑换内容。现在 Realm 段的服务端就是**真实 realm 服务**(GatewayRuntime +
/// ServiceFrame + 共享票据键),网关桩签发的也是真实 EnterRealm 票据;客户端
/// 不再有兑换桩——跑的就是生产 WireEnterRealmRedeemer。
///
/// 帧约定说明:TlsTcpTransport 自带 4 字节长度前缀(message 语义:
/// MessageReceived/ send() 收发的都是去帧的消息体),所以桩里直接按
/// 信封字节处理,不需要也不允许再套一层长度前缀。

#include "realmmesh/client/wire_login_transport.hpp"
#include "realmmesh/common/v1/envelope.pb.h"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/network/client/edge_client_connection.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/network/core/byte_buffer.hpp"
#include "realmmesh/network/http/http_server.hpp"
#include "realmmesh/network/transport/transport_factory.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/service_host/service_frame.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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

/// 测试侧共享票据键:网关桩用它签发真实 EnterRealm 票据,Realm 服务端用它
/// 验签(#46 realm 测试同款键)。
constexpr std::string_view kSharedTicketKeyHex =
    "0102030405060708090a0b0c0d0e0f10"
    "1112131415161718191a1b1c1d1e1f20";

/// 以测试侧共享密钥签一张真实 EnterRealm 票据。
[[nodiscard]] std::string mint_enter_realm_ticket(std::uint32_t realm_id) {
    const common::SessionTickets tickets{
        common::parse_ticket_key_hex(std::string{kSharedTicketKeyHex})};
    const auto ticket = tickets.issue(
        common::TicketPurpose::EnterRealm, /*account_id=*/42, realm_id,
        /*character_id=*/0, std::chrono::seconds{60});
    return std::string{reinterpret_cast<const char*>(ticket.data()),
                       ticket.size()};
}

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

    /// tickets/me 前 N 次回 401 + unauthorized_code(默认 2001 号牌过期)。
    std::atomic<int> unauthorized_remaining{1};
    /// 401 体里的错误码:2001 = 号牌过期(触发重取),其余是瞬时失败。
    std::atomic<int> unauthorized_code{2001};
    /// 401 之后、放行之前回 N 次 status=queued(位次 100)。
    std::atomic<int> queued_remaining{1};
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
                    401, "{\"code\":" + std::to_string(unauthorized_code.load()) +
                             ",\"message\":\"number token rejected\"}");
            }
            if (queued_remaining.load() > 0) {
                --queued_remaining;
                return json_response(
                    200,
                    R"({"status":"queued","position":100,)"
                    R"("estimated_wait_seconds":1})");
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
    /// 签发票据归属的 realm(负向用例注入别的 realm 以触发 Realm 侧拒绝)。
    std::atomic<std::uint32_t> ticket_realm_id{1};

    [[nodiscard]] std::string last_identity_token() const {
        std::lock_guard lock(mutex_);
        return last_identity_token_;
    }
    [[nodiscard]] std::string last_number_token() const {
        std::lock_guard lock(mutex_);
        return last_number_token_;
    }
    [[nodiscard]] std::string last_enter_realm_ticket() const {
        std::lock_guard lock(mutex_);
        return last_enter_realm_ticket_;
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
        const std::string ticket =
            mint_enter_realm_ticket(ticket_realm_id.load());
        granted.set_enter_realm_ticket(ticket);
        {
            std::lock_guard lock(mutex_);
            last_enter_realm_ticket_ = ticket;
        }
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
    std::string last_enter_realm_ticket_;
};

/// Realm 段的服务端:真实 realm 服务帧(#46 的 1304/1305 处理器)+ 与网关桩
/// 共享的票据键,监听真实 TLS。ALPN 与网关段一致 —— Realm 段就是 edge 段,
/// 所以这里不配 `.alpn`,取传输层默认值。
///
/// 自持 Logger 与临时日志目录;构造期要求共享票据键在环境里(ServiceFrame
/// 缺失即抛),故构造/析构自己管好这个 env(单进程内同一时刻至多一个实例)。
class RealmNode final {
public:
    RealmNode() {
        static_cast<void>(::setenv("REALMMESH_SESSION_TICKET_KEY",
                                   std::string{kSharedTicketKeyHex}.c_str(),
                                   1));
        log_directory_.emplace("realmmesh-client-realm-");
        observability::LoggerConfig logger_config;
        logger_config.file_path = log_directory_->path() / "realm.log";
        logger_.emplace(
            logger_config,
            observability::ServiceIdentity{.service_name = "realm"});
        runtime_.emplace(
            game::gateway::GatewayConfig{.transports = {realm_transport()}},
            game::gateway::GatewayRuntimeOptions{
                .inbound_capacity = 64,
                .outbound_capacity = 64,
                .io_poll_interval = milliseconds{1}});
        runtime_->start();
        frame_.emplace("realm", "127.0.0.1", 8443, 64,
                       service_host::EdgePipelineCaps{.conn_capacity = 4,
                                                      .fetch_capacity = 0});
    }

    ~RealmNode() {
        frame_.reset();
        runtime_->stop();
        static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
    }

    RealmNode(const RealmNode&) = delete;
    RealmNode& operator=(const RealmNode&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return runtime_->local_endpoints().front().port;
    }

    /// 驱动一帧业务(runtime 自己的 IO 线程负责传输轮询)。
    void poll(milliseconds timeout) {
        static_cast<void>(timeout);
        frame_->tick(*logger_, *runtime_, nullptr);
    }

private:
    [[nodiscard]] static network::TransportConfig realm_transport() {
        network::TransportConfig config{
            .name = "realm",
            .protocol = network::TransportProtocol::TlsTcp,
            .listen_address = "127.0.0.1",
            .listen_port = 0,
            .max_sessions = 16,
            .max_payload_size = 16 * 1024,
        };
        config.tls = network::TransportConfig::TlsServerIdentity{
            .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
            .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
        };
        return config;
    }

    std::optional<test_support::TemporaryDirectory> log_directory_;
    std::optional<observability::Logger> logger_;
    std::optional<game::gateway::GatewayRuntime> runtime_;
    std::optional<service_host::ServiceFrame> frame_;
};

/// 三个端共用一个驱动线程:HttpServer、网关传输与 realm 服务帧都是轮询
/// 模型(属主线程驱动),与生产服务同形。nullptr 表示本用例不使用该端
/// (端口级用例只驱动 realm 服务)。
class PollDriver final {
public:
    PollDriver(HttpStub* http, EdgeStub* edge, RealmNode* realm)
        : thread_([http, edge, realm](std::stop_token stop) {
              while (!stop.stop_requested()) {
                  if (http == nullptr && edge == nullptr) {
                      // 只剩 realm 服务帧时没有阻塞式 poll 兜底,补一个节拍
                      // 避免空转吃满 CPU。
                      std::this_thread::sleep_for(milliseconds{1});
                  }
                  if (http != nullptr) {
                      http->poll(milliseconds{1});
                  }
                  if (edge != nullptr) {
                      edge->poll(milliseconds{1});
                  }
                  if (realm != nullptr) {
                      realm->poll(milliseconds{1});
                  }
              }
          }) {}

private:
    std::jthread thread_;
};

/// 网关桩的传输配置(真实 TLS;ALPN 取传输层默认的 edge 线)。Realm 段的
/// 服务端由 RealmNode 自持 —— 它要挂真实 ServiceFrame,不是一条裸传输。
[[nodiscard]] network::TransportConfig gateway_config() {
    network::TransportConfig gateway{
        .name = "gateway-stub",
        .protocol = network::TransportProtocol::TlsTcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
    };
    gateway.tls = network::TransportConfig::TlsServerIdentity{
        .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
        .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
    };
    return gateway;
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
/// 网关段用 attach 帧拿到 1303(真实 EnterRealm 票据),Realm 段再起一次
/// 竞速,生产兑换口把票据兑换成 1305 并把链路推进 InGame。
TEST(WireLoginTransportIntegrationTest, DrivesLoginChainOverRealTls) {
    const std::array<network::TransportConfig, 1> configs{gateway_config()};
    auto transports = network::TransportFactory::create_enabled(configs);
    ASSERT_EQ(transports.size(), 1U);
    const auto gateway_port = transports.front()->local_endpoint().port;
    ASSERT_NE(gateway_port, 0);

    HttpStub http;
    RealmNode realm;
    EdgeStub edge(*transports.front(), realm.port());
    const PollDriver driver(&http, &edge, &realm);

    WireEndpoints endpoints;
    endpoints.login_verify_port = http.port();
    endpoints.queue_port = http.port();
    // 测试证书自签:显式关闭校验(生产默认校验)。
    endpoints.verify_peer = false;

    WireEnterRealmRedeemer redeemer;
    WireLoginTransport transport(endpoints, redeemer, fast_wire_options());

    LoginChain chain(transport, fast_chain_config(gateway_port));
    const auto result =
        chain.run("alice", "secret", Clock::now() + std::chrono::seconds{10});

    ASSERT_TRUE(result.succeeded())
        << "failure=" << chain_failure_name(result.failure)
        << " detail=" << result.detail;
    EXPECT_EQ(result.stage, LoginStage::InGame);
    EXPECT_EQ(result.number, 100U);

    // HTTP 段:verify 一次;查号三次(首查被 401+2001 判号牌过期→重取号,
    // 新号首查仍 queued,progress 追上号值后第三次拿到放行凭证);
    // progress 两次(首次未放行,第二次放行);tickets 两次(重取)。
    EXPECT_EQ(http.count_of("/v1/login/verify"), 1);
    EXPECT_EQ(http.count_of("/v1/queue/tickets"), 2);
    EXPECT_EQ(http.count_of("/v1/queue/progress"), 2);
    EXPECT_EQ(http.count_of("/v1/queue/tickets/me"), 3);

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

    // Realm 段:生产兑换口把网关签发的真实票据兑换成了 InGame;能到 InGame
    // 就说明真实 realm 服务验签通过并回了 1305(拒绝分支不可能到 InGame)。
    EXPECT_FALSE(chain.credentials().enter_realm_ticket.empty());
    EXPECT_EQ(chain.credentials().enter_realm_ticket,
              edge.last_enter_realm_ticket());
}

/// attach 被 1999 + 2001 拒(号牌过期):真实帧路径上同样自动重取号牌,
/// 不需要人类重新登录(spec §7 回退规则)。
TEST(WireLoginTransportIntegrationTest, AttachRejectionRetakesNumberToken) {
    const std::array<network::TransportConfig, 1> configs{gateway_config()};
    auto transports = network::TransportFactory::create_enabled(configs);
    ASSERT_EQ(transports.size(), 1U);
    const auto gateway_port = transports.front()->local_endpoint().port;

    HttpStub http;
    http.unauthorized_remaining.store(0);
    // 首查直接放行,不必等 progress。
    http.queued_remaining.store(0);
    RealmNode realm;
    EdgeStub edge(*transports.front(), realm.port());
    edge.reject_attach_remaining.store(1);
    const PollDriver driver(&http, &edge, &realm);

    WireEndpoints endpoints;
    endpoints.login_verify_port = http.port();
    endpoints.queue_port = http.port();
    endpoints.verify_peer = false;

    WireEnterRealmRedeemer redeemer;
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
    // 第二次 attach 交付的真实票据同样被 Realm 段受理。
    EXPECT_FALSE(chain.credentials().enter_realm_ticket.empty());
}

/// 401 不一律等于号牌过期:只有错误码 2001 才触发自动重取(spec §5.1
/// 错误模型);其余 401 是瞬时失败,交给下一次轮询而不是回排队重取。
TEST(WireLoginTransportIntegrationTest, NonExpiryUnauthorizedIsTransient) {
    HttpStub http;
    http.unauthorized_remaining.store(1);
    http.unauthorized_code.store(1001);
    std::jthread driver([&http](std::stop_token stop) {
        while (!stop.stop_requested()) {
            http.poll(milliseconds{1});
        }
    });

    WireEndpoints endpoints;
    endpoints.login_verify_port = http.port();
    endpoints.queue_port = http.port();
    endpoints.verify_peer = false;

    WireEnterRealmRedeemer redeemer;
    WireLoginTransport transport(endpoints, redeemer, fast_wire_options());
    const auto me =
        transport.ticket_me("number-token-1", Clock::now() + std::chrono::seconds{2});

    EXPECT_FALSE(me.status.ok);
    EXPECT_FALSE(me.status.credential_expired);
    EXPECT_EQ(me.status.failure, ChainFailure::ProgressFailed);
}

/// 帧级替身:记录写出的字节,并按需回放罐头帧(含长度前缀)。不起 socket,
/// 只验兑换口对「一帧一答」的处理;canned 空 = 对端无数据(超时/断开路径)。
class CannedStream final : public net_client::ISecureByteStream {
public:
    std::vector<std::byte> canned;
    std::vector<std::byte> written;

    [[nodiscard]] bool write_all(std::span<const std::byte> data,
                                 net_client::StreamDeadline deadline) override {
        static_cast<void>(deadline);
        written.insert(written.end(), data.begin(), data.end());
        return true;
    }

    [[nodiscard]] std::optional<std::size_t> read_some(
        std::span<std::byte> out, net_client::StreamDeadline deadline) override {
        static_cast<void>(deadline);
        if (canned.empty()) {
            return std::nullopt;
        }
        const std::size_t count = std::min(out.size(), canned.size());
        std::copy_n(canned.begin(), count, out.begin());
        canned.erase(canned.begin(),
                     canned.begin() + static_cast<std::ptrdiff_t>(count));
        return count;
    }

    void shutdown() override {}
};

/// 一帧的线上字节(补长度前缀,与 EdgeClientConnection 的收发同款)。
[[nodiscard]] std::vector<std::byte> framed(
    std::span<const std::byte> payload) {
    return network::LengthFieldCodec{net_client::kMaxEdgeFramePayload}.encode(
        payload);
}

/// 反解兑换口写出的请求:去前缀 → 信封 → 1304 请求体。取不到即 nullopt。
[[nodiscard]] std::optional<common::EnterRealm> parse_enter_realm_request(
    std::span<const std::byte> written) {
    network::ByteBuffer buffer;
    buffer.append(written);
    const auto decoded = network::LengthFieldCodec{
        net_client::kMaxEdgeFramePayload}.try_decode(buffer);
    if (decoded.status != network::DecodeStatus::FrameReady) {
        return std::nullopt;
    }
    const auto message_id = common::edge_message_id(decoded.payload);
    if (!message_id.has_value() ||
        *message_id != edge_v1::MESSAGE_ID_C2S_ENTER_REALM) {
        return std::nullopt;
    }
    return common::decode_enter_realm(decoded.payload);
}

/// 写出的必须是一帧可解析的 1304(票据原样进 payload);收到 1305 即成功。
TEST(WireEnterRealmRedeemerTest, Sends1304AndAccepts1305) {
    common::EnterRealmAccepted accepted;
    accepted.set_account_id(42);
    CannedStream stream;
    stream.canned = framed(common::encode(accepted));

    WireEnterRealmRedeemer redeemer;
    const auto status =
        redeemer.redeem(stream, "ert-1", Clock::now() + std::chrono::seconds{1});

    ASSERT_TRUE(status.ok) << status.detail;
    const auto request = parse_enter_realm_request(stream.written);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->enter_realm_ticket(), "ert-1");
}

/// 1999 EdgeError(3002 无效票据)→ EnterRealmRejected,detail 带服务端错误码。
TEST(WireEnterRealmRedeemerTest, MapsEdgeErrorToEnterRealmRejected) {
    common::EdgeError error;
    error.set_code(static_cast<std::uint32_t>(
        common::edge_error_invalid_enter_realm_ticket));
    error.set_message("invalid enter realm ticket");
    CannedStream stream;
    stream.canned = framed(common::encode(error));

    WireEnterRealmRedeemer redeemer;
    const auto status =
        redeemer.redeem(stream, "ert-1", Clock::now() + std::chrono::seconds{1});

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.failure, ChainFailure::EnterRealmRejected);
    EXPECT_NE(status.detail.find("3002"), std::string::npos) << status.detail;
}

/// 帧取不到信封(不可解析)→ 坏帧,同样是 EnterRealmRejected。
TEST(WireEnterRealmRedeemerTest, MapsBadFrameToEnterRealmRejected) {
    const std::vector<std::byte> garbage{std::byte{0xFF}};
    CannedStream stream;
    stream.canned = framed(garbage);

    WireEnterRealmRedeemer redeemer;
    const auto status =
        redeemer.redeem(stream, "ert-1", Clock::now() + std::chrono::seconds{1});

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.failure, ChainFailure::EnterRealmRejected);
    EXPECT_NE(status.detail.find("坏帧"), std::string::npos) << status.detail;
}

/// 信封解得出 1305、但帧体不是合法 EnterRealmAccepted → 同样是坏帧:
/// 消息号对不代表内容可信,不得当成功收下。
TEST(WireEnterRealmRedeemerTest, MapsUndecodableAcceptedToEnterRealmRejected) {
    ::realmmesh::protocol::common::v1::Envelope envelope;
    envelope.set_protocol_version(common::kEdgeProtocolVersion);
    envelope.set_message_id(
        static_cast<std::uint32_t>(edge_v1::MESSAGE_ID_S2C_ENTER_REALM_ACCEPTED));
    envelope.set_request_id(0);
    envelope.set_payload("\xFF");  // 非法 wire type:EnterRealmAccepted 解不出
    std::string bytes;
    ASSERT_TRUE(envelope.SerializeToString(&bytes));
    const auto* begin = reinterpret_cast<const std::byte*>(bytes.data());

    CannedStream stream;
    stream.canned = framed({begin, begin + bytes.size()});

    WireEnterRealmRedeemer redeemer;
    const auto status =
        redeemer.redeem(stream, "ert-1", Clock::now() + std::chrono::seconds{1});

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.failure, ChainFailure::EnterRealmRejected);
    EXPECT_NE(status.detail.find("坏帧"), std::string::npos) << status.detail;
}

/// 对端无数据(超时/断开)→ EnterRealmRejected:不新增超时分型,请求仍然
/// 被写出(证明确实尝试过兑换)。
TEST(WireEnterRealmRedeemerTest, MapsTimeoutToEnterRealmRejected) {
    CannedStream stream;  // canned 空:读即刻判空

    WireEnterRealmRedeemer redeemer;
    const auto status =
        redeemer.redeem(stream, "ert-1", Clock::now() + std::chrono::seconds{1});

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.failure, ChainFailure::EnterRealmRejected);
    EXPECT_TRUE(parse_enter_realm_request(stream.written).has_value())
        << status.detail;
}

/// Realm 拒绝票据:端口把 1999/3002 明确映射为 EnterRealmRejected。断言取
/// 端口级——链路终态的分型会被后续回退阶段覆盖,不是这个映射的稳定观测点。
/// 服务端是真实 realm 服务:realm_id 2 的票据在单 Realm 拓扑下被判 3002。
TEST(WireLoginTransportIntegrationTest, RealmRejectsForeignTicketAtPortLevel) {
    RealmNode realm;
    const PollDriver driver(nullptr, nullptr, &realm);

    WireEndpoints endpoints;
    endpoints.verify_peer = false;
    WireEnterRealmRedeemer redeemer;
    WireLoginTransport transport(endpoints, redeemer, fast_wire_options());

    const std::array<net_client::EndpointCandidate, 2> candidates{
        net_client::EndpointCandidate{
            .protocol = network::TransportProtocol::Quic,
            .host = "127.0.0.1",
            .port = realm.port(),
            .priority = 0,
        },
        net_client::EndpointCandidate{
            .protocol = network::TransportProtocol::TlsTcp,
            .host = "127.0.0.1",
            .port = realm.port(),
            .priority = 1,
        }};
    const auto deadline = Clock::now() + std::chrono::seconds{5};
    const auto connected = transport.connect_realm(candidates, deadline);
    ASSERT_TRUE(connected.ok) << connected.detail;

    const auto status =
        transport.enter_realm(mint_enter_realm_ticket(2), deadline);

    EXPECT_FALSE(status.ok);
    EXPECT_EQ(status.failure, ChainFailure::EnterRealmRejected);
    EXPECT_NE(status.detail.find("3002"), std::string::npos) << status.detail;
}

}  // namespace
}  // namespace realm::client
