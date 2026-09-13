#include "realmmesh/client/login_chain.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace realm::client {
namespace {

using std::chrono::milliseconds;

namespace net_client = ::realm::network::client;

[[nodiscard]] PortStatus failure_of(ChainFailure failure,
                                    std::string detail = {},
                                    bool credential_expired = false) {
    return PortStatus::error(failure, std::move(detail), credential_expired);
}

template <typename T>
[[nodiscard]] PortValue<T> ok_value(T value) {
    PortValue<T> result;
    result.value = std::move(value);
    result.status = PortStatus::success();
    return result;
}

template <typename T>
[[nodiscard]] PortValue<T> failed_value(ChainFailure failure,
                                       std::string detail = {},
                                       bool credential_expired = false) {
    PortValue<T> result;
    result.status = failure_of(failure, std::move(detail), credential_expired);
    return result;
}

[[nodiscard]] PortValue<VerifyResult> verified(std::string identity_token) {
    VerifyResult value;
    value.identity_token = std::move(identity_token);
    return ok_value(std::move(value));
}

[[nodiscard]] PortValue<TicketResult> ticketed(std::string token,
                                              std::uint64_t number) {
    TicketResult value;
    value.queue_number_token = std::move(token);
    value.number = number;
    return ok_value(std::move(value));
}

[[nodiscard]] PortValue<ProgressResult> progress(std::uint64_t released,
                                                double admit_rate) {
    return ok_value(ProgressResult{released, admit_rate});
}

[[nodiscard]] PortValue<TicketMeResult> queued_at(std::uint64_t position) {
    TicketMeResult value;
    value.position = position;
    return ok_value(std::move(value));
}

[[nodiscard]] PortValue<TicketMeResult> admitted_with(
    std::string grant_token,
    std::chrono::seconds grace = std::chrono::seconds{300}) {
    TicketMeResult value;
    value.admitted = true;
    value.admitted_token = std::move(grant_token);
    value.admit_grace = grace;
    return ok_value(std::move(value));
}

[[nodiscard]] PortValue<HandoffResult> handed_off(std::string ticket,
                                                 std::uint16_t realm_port) {
    HandoffResult value;
    value.enter_realm_ticket = std::move(ticket);
    value.realm_endpoints.push_back(net_client::EndpointCandidate{
        .protocol = network::TransportProtocol::Quic,
        .host = "127.0.0.1",
        .port = 1,
        .priority = 1,
    });
    value.realm_endpoints.push_back(net_client::EndpointCandidate{
        .protocol = network::TransportProtocol::TlsTcp,
        .host = "127.0.0.1",
        .port = realm_port,
        .priority = 2,
    });
    return ok_value(std::move(value));
}

/// 脚本化传输口:每个方法消费一条脚本(用尽后重复最后一条),并计数。
/// 只做编排验证,不碰网络——网络路径由集成档覆盖。
class ScriptedTransport final : public LoginChainTransport {
public:
    /// 脚本用尽后重复最后一条;空脚本返回默认值(测试写错时不崩)。
    [[nodiscard]] static const auto& pick(const auto& script, int calls) {
        if (script.empty()) {
            static const std::decay_t<decltype(script.front())> fallback{};
            return fallback;
        }
        const auto index = static_cast<std::size_t>(
            std::min<int>(calls, static_cast<int>(script.size()) - 1));
        return script.at(index);
    }

    PortValue<VerifyResult> verify(std::string_view account,
                                   std::string_view credential,
                                   TimePoint deadline) override {
        static_cast<void>(deadline);
        last_account = std::string{account};
        last_credential = std::string{credential};
        return pick(verify_results, verify_calls++);
    }

    PortValue<TicketResult> take_ticket(std::string_view identity_token,
                                        TimePoint deadline) override {
        static_cast<void>(deadline);
        last_identity_token = std::string{identity_token};
        return pick(ticket_results, ticket_calls++);
    }

    PortValue<ProgressResult> poll_progress(TimePoint deadline) override {
        static_cast<void>(deadline);
        return pick(progress_results, progress_calls++);
    }

    PortValue<TicketMeResult> ticket_me(std::string_view queue_number_token,
                                        TimePoint deadline) override {
        static_cast<void>(deadline);
        last_number_token = std::string{queue_number_token};
        return pick(me_results, me_calls++);
    }

    PortStatus connect_gateway(
        std::span<const net_client::EndpointCandidate> candidates,
        TimePoint deadline) override {
        static_cast<void>(deadline);
        gateway_candidates.assign(candidates.begin(), candidates.end());
        return pick(gateway_results, gateway_calls++);
    }

    PortStatus attach(std::string_view identity_token,
                      std::string_view queue_number_token,
                      TimePoint deadline) override {
        static_cast<void>(deadline);
        last_identity_token = std::string{identity_token};
        last_number_token = std::string{queue_number_token};
        return pick(attach_results, attach_calls++);
    }

    PortValue<HandoffResult> await_handoff(TimePoint deadline) override {
        static_cast<void>(deadline);
        return pick(handoff_results, handoff_calls++);
    }

    PortStatus connect_realm(
        std::span<const net_client::EndpointCandidate> candidates,
        TimePoint deadline) override {
        static_cast<void>(deadline);
        realm_candidates.assign(candidates.begin(), candidates.end());
        return pick(realm_connect_results, realm_connect_calls++);
    }

    PortStatus enter_realm(std::string_view enter_realm_ticket,
                           TimePoint deadline) override {
        static_cast<void>(deadline);
        last_enter_realm_ticket = std::string{enter_realm_ticket};
        return pick(realm_redeem_results, realm_redeem_calls++);
    }

    void drop_connections() override { ++drop_calls; }

    std::vector<PortValue<VerifyResult>> verify_results{verified("identity-1")};
    std::vector<PortValue<TicketResult>> ticket_results{
        ticketed("number-token-1", 100)};
    std::vector<PortValue<ProgressResult>> progress_results{progress(0, 10.0)};
    std::vector<PortValue<TicketMeResult>> me_results{queued_at(100)};
    std::vector<PortStatus> gateway_results{PortStatus::success()};
    std::vector<PortStatus> attach_results{PortStatus::success()};
    std::vector<PortValue<HandoffResult>> handoff_results{
        handed_off("enter-realm-ticket-1", 9000)};
    std::vector<PortStatus> realm_connect_results{PortStatus::success()};
    std::vector<PortStatus> realm_redeem_results{PortStatus::success()};

    int verify_calls{0};
    int ticket_calls{0};
    int progress_calls{0};
    int me_calls{0};
    int gateway_calls{0};
    int attach_calls{0};
    int handoff_calls{0};
    int realm_connect_calls{0};
    int realm_redeem_calls{0};
    int drop_calls{0};

    std::string last_account;
    std::string last_credential;
    std::string last_identity_token;
    std::string last_number_token;
    std::string last_enter_realm_ticket;
    std::vector<net_client::EndpointCandidate> gateway_candidates;
    std::vector<net_client::EndpointCandidate> realm_candidates;
};

/// 压缩时标:分档/退避的结构不变,但不用为单测等 2s/5s 的真实间隔。
[[nodiscard]] LoginChainConfig fast_config() {
    LoginChainConfig config;
    config.poll.initial_interval = milliseconds{20};
    config.poll.far_interval = milliseconds{20};
    config.poll.near_interval = milliseconds{1};
    config.poll.min_interval = milliseconds{0};
    config.poll.backoff_cap = milliseconds{40};
    config.poll.jitter_ratio = 0.0;
    config.gateway_retry_delay = milliseconds{1};
    config.realm_retry_delay = milliseconds{1};
    config.gateway_endpoints.push_back(net_client::EndpointCandidate{
        .protocol = network::TransportProtocol::TlsTcp,
        .host = "127.0.0.1",
        .port = 8000,
        .priority = 1,
    });
    return config;
}

[[nodiscard]] TimePoint few_seconds_from_now() {
    return Clock::now() + std::chrono::seconds{5};
}

/// 七态推进:verifying → queued → admitted → gateway_connecting →
/// handoff_received → realm_connecting → in_game,且凭据全程只在内存里
/// 逐段补齐(spec §7)。
TEST(LoginChainTest, HappyPathWalksSevenStatesAndFillsCredentials) {
    ScriptedTransport transport;
    transport.progress_results = {progress(0, 10.0), progress(100, 10.0)};
    transport.me_results = {queued_at(100), admitted_with("grant-1")};

    LoginChain chain(transport, fast_config());
    std::vector<LoginStage> trace;
    chain.on_stage_change(
        [&trace](LoginStage stage) { trace.push_back(stage); });

    const auto result = chain.run("alice", "secret", few_seconds_from_now());

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(result.stage, LoginStage::InGame);
    EXPECT_EQ(result.failure, ChainFailure::None);
    EXPECT_EQ(result.number, 100U);

    const std::vector<LoginStage> expected{
        LoginStage::Verifying,
        LoginStage::Queued,
        LoginStage::Admitted,
        LoginStage::GatewayConnecting,
        LoginStage::HandoffReceived,
        LoginStage::RealmConnecting,
        LoginStage::InGame,
    };
    EXPECT_EQ(trace, expected);

    const auto& credentials = chain.credentials();
    EXPECT_EQ(credentials.identity_token, "identity-1");
    EXPECT_EQ(credentials.queue_number_token, "number-token-1");
    EXPECT_EQ(credentials.number, 100U);
    EXPECT_EQ(credentials.admitted_token, "grant-1");
    EXPECT_EQ(credentials.enter_realm_ticket, "enter-realm-ticket-1");

    EXPECT_EQ(transport.last_account, "alice");
    EXPECT_EQ(transport.last_credential, "secret");
    // admit 阶段的 attach 用重签号牌,不是原始号牌。
    EXPECT_EQ(transport.last_number_token, "grant-1");
    EXPECT_EQ(transport.last_enter_realm_ticket, "enter-realm-ticket-1");

    EXPECT_EQ(transport.verify_calls, 1);
    EXPECT_EQ(transport.ticket_calls, 1);
    // 首查 tickets/me(位次以查询为准)+ progress 追上号值后的兜底查。
    EXPECT_EQ(transport.progress_calls, 2);
    EXPECT_EQ(transport.me_calls, 2);
    EXPECT_EQ(transport.gateway_calls, 1);
    EXPECT_EQ(transport.attach_calls, 1);
    EXPECT_EQ(transport.handoff_calls, 1);
    EXPECT_EQ(transport.realm_connect_calls, 1);
    EXPECT_EQ(transport.realm_redeem_calls, 1);
    // 交付到手即释放网关会话(票据已在手):不留悬挂连接。
    EXPECT_EQ(transport.drop_calls, 1);

    // Realm 段竞速用的是 1303 下发的端点(QUIC 主 + TLS/TCP 降级两项)。
    ASSERT_EQ(transport.realm_candidates.size(), 2U);
    EXPECT_EQ(transport.realm_candidates.at(0).priority, 1U);
    EXPECT_EQ(transport.realm_candidates.at(1).port, 9000);
}

/// verifying 失败即回 idle(spec §7):不再取号。
TEST(LoginChainTest, VerifyRejectionReturnsToIdle) {
    ScriptedTransport transport;
    transport.verify_results = {
        failed_value<VerifyResult>(ChainFailure::VerifyRejected, "凭据不符")};

    LoginChain chain(transport, fast_config());
    std::vector<LoginStage> trace;
    chain.on_stage_change(
        [&trace](LoginStage stage) { trace.push_back(stage); });

    const auto result = chain.run("alice", "wrong", few_seconds_from_now());

    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(result.stage, LoginStage::Idle);
    EXPECT_EQ(result.failure, ChainFailure::VerifyRejected);
    EXPECT_EQ(result.detail, "凭据不符");
    EXPECT_EQ(transport.ticket_calls, 0);
    EXPECT_EQ(trace,
              (std::vector<LoginStage>{LoginStage::Verifying,
                                       LoginStage::Idle}));
}

/// 号牌过期(401/2001)→ 自动重取,不需要人类介入(spec §7)。
TEST(LoginChainTest, ExpiredNumberTokenRetakesTicketAutomatically) {
    ScriptedTransport transport;
    transport.ticket_results = {ticketed("number-token-1", 100),
                               ticketed("number-token-2", 200)};
    transport.progress_results = {progress(100, 10.0), progress(200, 10.0)};
    transport.me_results = {
        failed_value<TicketMeResult>(ChainFailure::TicketRejected,
                                     "号牌已过期",
                                     /*credential_expired=*/true),
        admitted_with("grant-2"),
    };

    LoginChain chain(transport, fast_config());
    std::vector<LoginStage> trace;
    chain.on_stage_change(
        [&trace](LoginStage stage) { trace.push_back(stage); });

    const auto result = chain.run("alice", "secret", few_seconds_from_now());

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(transport.ticket_calls, 2);
    EXPECT_EQ(chain.credentials().number, 200U);
    EXPECT_EQ(chain.credentials().queue_number_token, "number-token-2");
    // 重取等于新排队:Queued 出现两次。
    EXPECT_EQ(std::count(trace.begin(), trace.end(), LoginStage::Queued), 2);
}

/// 宽限内 attach 被拒(2001)→ 回排队重取号牌。
TEST(LoginChainTest, ExpiredNumberTokenOnAttachRetakesTicket) {
    ScriptedTransport transport;
    transport.ticket_results = {ticketed("number-token-1", 100),
                               ticketed("number-token-2", 200)};
    transport.progress_results = {progress(100, 10.0), progress(200, 10.0)};
    transport.me_results = {admitted_with("grant-1"),
                            admitted_with("grant-2")};
    transport.attach_results = {
        failure_of(ChainFailure::AttachRejected, "号牌过期",
                   /*credential_expired=*/true),
        PortStatus::success(),
    };

    LoginChain chain(transport, fast_config());
    const auto result = chain.run("alice", "secret", few_seconds_from_now());

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(transport.ticket_calls, 2);
    EXPECT_EQ(transport.attach_calls, 2);
}

/// 交付相位收到坏帧/被拒:宽限内回网关重入,不丢号。
TEST(LoginChainTest, HandoffRejectionRetriesGatewayWithinGrace) {
    ScriptedTransport transport;
    transport.progress_results = {progress(100, 10.0)};
    transport.me_results = {admitted_with("grant-1")};
    transport.handoff_results = {
        failed_value<HandoffResult>(ChainFailure::HandoffRejected, "坏帧"),
        handed_off("enter-realm-ticket-2", 9000),
    };

    LoginChain chain(transport, fast_config());
    const auto result = chain.run("alice", "secret", few_seconds_from_now());

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(transport.gateway_calls, 2);
    EXPECT_EQ(transport.handoff_calls, 2);
    EXPECT_EQ(transport.ticket_calls, 1);
}

/// 网关连不上:宽限内按重入节奏重试,成功后照常走完;不计失败于号牌。
TEST(LoginChainTest, GatewayFailureRetriesWithinGraceThenSucceeds) {
    ScriptedTransport transport;
    transport.progress_results = {progress(100, 10.0)};
    transport.me_results = {admitted_with("grant-1")};
    transport.gateway_results = {
        failure_of(ChainFailure::GatewayConnectFailed, "网络不可达"),
        failure_of(ChainFailure::GatewayConnectFailed, "网络不可达"),
        PortStatus::success(),
    };

    LoginChain chain(transport, fast_config());
    std::vector<LoginStage> trace;
    chain.on_stage_change(
        [&trace](LoginStage stage) { trace.push_back(stage); });

    const auto result = chain.run("alice", "secret", few_seconds_from_now());

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(transport.gateway_calls, 3);
    EXPECT_EQ(transport.handoff_calls, 1);
    EXPECT_EQ(transport.ticket_calls, 1);
    EXPECT_EQ(std::count(trace.begin(), trace.end(),
                         LoginStage::GatewayConnecting),
              3);
}

/// 宽限耗尽:号牌视同过期,回排队重取(spec §7)。
TEST(LoginChainTest, ExhaustedAdmitGraceRetakesTicket) {
    ScriptedTransport transport;
    auto config = fast_config();
    config.admit_grace_fallback = std::chrono::seconds{0};
    transport.ticket_results = {ticketed("number-token-1", 100),
                               ticketed("number-token-2", 200)};
    transport.progress_results = {progress(100, 10.0), progress(200, 10.0)};
    transport.me_results = {admitted_with("grant-1", std::chrono::seconds{0}),
                            admitted_with("grant-2")};
    transport.gateway_results = {
        failure_of(ChainFailure::GatewayConnectFailed, "网络不可达"),
        PortStatus::success(),
    };

    LoginChain chain(transport, config);
    const auto result = chain.run("alice", "secret", few_seconds_from_now());

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(transport.ticket_calls, 2);
    EXPECT_EQ(chain.credentials().number, 200U);
}

/// Realm 段失败且 60s 票窗耗尽:回网关重入换新票据,号牌不动
/// (spec §7「Realm 直连失败 → EnterRealm 60s 内重试」的窗口语义)。
TEST(LoginChainTest, RealmTicketWindowExhaustionReturnsToGateway) {
    ScriptedTransport transport;
    auto config = fast_config();
    config.enter_realm_ttl = std::chrono::seconds{0};
    transport.progress_results = {progress(100, 10.0)};
    transport.me_results = {admitted_with("grant-1")};
    transport.realm_redeem_results = {
        failure_of(ChainFailure::EnterRealmRejected, "兑换被拒"),
        PortStatus::success(),
    };

    LoginChain chain(transport, config);
    std::vector<LoginStage> trace;
    chain.on_stage_change(
        [&trace](LoginStage stage) { trace.push_back(stage); });

    const auto result = chain.run("alice", "secret", few_seconds_from_now());

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(transport.ticket_calls, 1);
    EXPECT_EQ(transport.gateway_calls, 2);
    EXPECT_EQ(transport.handoff_calls, 2);
    EXPECT_EQ(transport.realm_redeem_calls, 2);
    // 两次 realm_connecting:第二次才拿到游戏会话。
    EXPECT_EQ(std::count(trace.begin(), trace.end(),
                         LoginStage::RealmConnecting),
              2);
}

/// 轮询节奏取自 AdaptivePoller:位次在近档时用 near_interval(分档本身
/// 由 adaptive_poller_test 钉死,这里只验证链路接上了它)。
TEST(LoginChainTest, ChainUsesPollerIntervalForWaiting) {
    ScriptedTransport transport;
    auto config = fast_config();
    config.poll.near_interval = milliseconds{3};
    config.poll.initial_interval = milliseconds{50};
    // 号 100,released 100 → 位次 0(近档);第二次查询才放行。
    transport.progress_results = {progress(100, 10.0), progress(100, 10.0)};
    transport.me_results = {queued_at(0), admitted_with("grant-1")};

    LoginChain chain(transport, config);
    const auto result = chain.run("alice", "secret", few_seconds_from_now());

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(chain.last_poll_interval(), milliseconds{3});
}

/// ETA 本地插值:位次 / 放行速率,进度查询失败时保留最近一次估计。
TEST(LoginChainTest, EtaComesFromLatestProgress) {
    ScriptedTransport transport;
    transport.ticket_results = {ticketed("number-token-1", 500)};
    transport.progress_results = {
        progress(0, 25.0),
        failed_value<ProgressResult>(ChainFailure::ProgressFailed, "瞬时失败"),
    };
    transport.me_results = {queued_at(500), admitted_with("grant-1")};

    LoginChain chain(transport, fast_config());
    const auto result = chain.run("alice", "secret", few_seconds_from_now());

    EXPECT_TRUE(result.succeeded());
    ASSERT_TRUE(result.eta.has_value());
    EXPECT_EQ(*result.eta, std::chrono::seconds{20});
}

/// 首查 tickets/me:号值已进放行区间时立刻拿凭证,不必先白等一个轮询
/// 间隔(spec §5.1 #4「首查」)。
TEST(LoginChainTest, FirstQueryAdmitsWithoutWaitingForProgress) {
    ScriptedTransport transport;
    transport.progress_results = {};
    transport.me_results = {admitted_with("grant-1")};

    LoginChain chain(transport, fast_config());
    const auto result = chain.run("alice", "secret", few_seconds_from_now());

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(result.stage, LoginStage::InGame);
    EXPECT_EQ(transport.progress_calls, 0);
    EXPECT_EQ(transport.me_calls, 1);
    EXPECT_EQ(transport.last_number_token, "grant-1");
}

/// 号牌被网关判过期(attach 2001)→ 回排队重取前先释放本段的 Edge
/// Session:重取号后连接没有任何复用价值,留着就是悬挂连接。
TEST(LoginChainTest, CredentialExpiryOnAttachReleasesGatewaySession) {
    ScriptedTransport transport;
    transport.ticket_results = {ticketed("number-token-1", 100),
                               ticketed("number-token-2", 200)};
    transport.progress_results = {progress(100, 10.0), progress(200, 10.0)};
    transport.me_results = {admitted_with("grant-1"),
                            admitted_with("grant-2")};
    transport.attach_results = {
        failure_of(ChainFailure::AttachRejected, "号牌过期",
                   /*credential_expired=*/true),
        PortStatus::success(),
    };

    LoginChain chain(transport, fast_config());
    const auto result = chain.run("alice", "secret", few_seconds_from_now());

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(transport.ticket_calls, 2);
    // 两次释放:重取号前释放旧会话 + 交付到手后释放(交付成功路径)。
    // 重取路径若不释放,这里只会看到 1 次。
    EXPECT_EQ(transport.drop_calls, 2);
}

/// 总窗口耗尽:排队等不到放行 → AdmitTimeout 回 idle(spec §7),期间
/// 反复轮询而不是查一次就放弃。
TEST(LoginChainTest, AdmitTimeoutReturnsToIdle) {
    ScriptedTransport transport;
    transport.ticket_results = {ticketed("number-token-1", 1000)};
    transport.progress_results = {progress(0, 1.0)};
    transport.me_results = {queued_at(1000)};

    // 档位压到 1ms、窗口给足:首查占掉第一轮,轮询节奏要在窗口内跑出
    // 多次才说明"反复轮询"(20ms 档 + 60ms 窗口在慢机器上只够一次,断言
    // 会变成时序彩票——macOS CI 上就栽在这)。
    auto config = fast_config();
    config.poll.initial_interval = milliseconds{1};
    config.poll.far_interval = milliseconds{1};

    LoginChain chain(transport, config);
    const auto result = chain.run("alice", "secret",
                                  Clock::now() + milliseconds{250});

    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(result.stage, LoginStage::Idle);
    EXPECT_EQ(result.failure, ChainFailure::AdmitTimeout);
    EXPECT_GT(transport.progress_calls, 1);
    // 首查一次;此后 progress 报 released=0,号值 1000 没进放行区间,
    // 不再查号。
    EXPECT_EQ(transport.me_calls, 1);
    EXPECT_EQ(transport.gateway_calls, 0);
}

/// 阶段名的线上口径(spec §7 七态命名):回调拿到的状态与名字一一对应。
TEST(LoginChainTest, StageNamesMatchSpecVocabulary) {
    EXPECT_EQ(login_stage_name(LoginStage::Idle), "idle");
    EXPECT_EQ(login_stage_name(LoginStage::Verifying), "verifying");
    EXPECT_EQ(login_stage_name(LoginStage::Queued), "queued");
    EXPECT_EQ(login_stage_name(LoginStage::Admitted), "admitted");
    EXPECT_EQ(login_stage_name(LoginStage::GatewayConnecting),
              "gateway_connecting");
    EXPECT_EQ(login_stage_name(LoginStage::HandoffReceived),
              "handoff_received");
    EXPECT_EQ(login_stage_name(LoginStage::RealmConnecting),
              "realm_connecting");
    EXPECT_EQ(login_stage_name(LoginStage::InGame), "in_game");
    EXPECT_EQ(chain_failure_name(ChainFailure::HandoffRejected),
              "handoff_rejected");
    EXPECT_EQ(chain_failure_name(ChainFailure::EnterRealmRejected),
              "enter_realm_rejected");
}

}  // namespace
}  // namespace realm::client
