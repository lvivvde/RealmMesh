#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/gateway/admission_consumption_store.hpp"
#include "realmmesh/game/gateway/gateway_admission.hpp"
#include "realmmesh/game/gateway/gateway_ingress.hpp"
#include "realmmesh/game/gateway/gateway_login_config.hpp"
#include "realmmesh/game/gateway/gateway_login_pipeline.hpp"
#include "realmmesh/game/gateway/gateway_primary_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace realm::game::gateway {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view identity_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view grant_seed_hex =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view consumption_digest_key_hex =
    "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7";
constexpr std::string_view ticket_key_hex =
    "0102030405060708090a0b0c0d0e0f10"
    "1112131415161718191a1b1c1d1e1f20";

/// 一枚结构合法(三段式、签名段 86 字符 base64url)但毫无价值的 attach 帧:
/// 限流发生在 protobuf/JWS 结构检查**之前**,而结构检查在限流之后——两者
/// 的先后正是本用例的观测点,所以凭据必须能过结构关。
[[nodiscard]] std::vector<std::byte> attach_payload(std::uint64_t request_id) {
    common::EdgeAttach attach;
    attach.set_identity_token(
        "eyJhbGciOiJFZERTQSJ9.eyJhIjoxfQ." + std::string(86, 'A'));
    attach.set_admission_grant(
        "eyJhbGciOiJFZERTQSJ9.eyJiIjoyfQ." + std::string(86, 'B'));
    return common::encode(attach, request_id);
}

/// 单来源令牌桶:突发额度耗尽后返回可重试的限流,连续超限升级为
/// sustained abuse(连接关闭),且桶按来源隔离——这正是 #88 里 100 个
/// 机器人同源 attach 撞上的那条真实护栏。
TEST(GatewayIngressThrottleTest, SingleSourceBucketThrottlesThenFlagsAbuse) {
    GatewayCredentialIngress ingress(GatewayCredentialIngressConfig{
        .source_rate_per_second = 1,
        .source_burst = 2,
        .source_throttle_close_after = 3,
    });
    const auto payload = attach_payload(1);
    const auto now =
        std::chrono::steady_clock::time_point{std::chrono::seconds{1'000}};

    EXPECT_EQ(
        ingress.inspect_attach("127.0.0.1", 1, payload, now).status,
        GatewayIngressStatus::Allowed);
    EXPECT_EQ(
        ingress.inspect_attach("127.0.0.1", 2, payload, now).status,
        GatewayIngressStatus::Allowed);

    const auto throttled = ingress.inspect_attach("127.0.0.1", 3, payload, now);
    EXPECT_EQ(throttled.status, GatewayIngressStatus::SourceThrottled);
    // 额度桶见底时给出 ≥1s 的重试窗口(不是 0:客户端不得忙等重试)。
    EXPECT_GE(throttled.retry_after, 1s);

    EXPECT_EQ(
        ingress.inspect_attach("127.0.0.1", 4, payload, now).status,
        GatewayIngressStatus::SourceThrottled);
    EXPECT_EQ(
        ingress.inspect_attach("127.0.0.1", 5, payload, now).status,
        GatewayIngressStatus::SustainedAbuse);

    // 桶按来源隔离:另一个来源不受同一 IP 的消耗影响。
    EXPECT_EQ(
        ingress.inspect_attach("127.0.0.2", 1, payload, now).status,
        GatewayIngressStatus::Allowed);

    const auto counters = ingress.counters();
    EXPECT_EQ(counters.source_throttled, 2U);
    EXPECT_EQ(counters.sustained_abuse, 1U);
}

/// 最简拉取桩:本用例的 attach 在凭据校验前就被守卫拦下,拉取永不发生。
class UnusedFetchPort final : public AccountFetchPort {
public:
    [[nodiscard]] AccountFetchSubmitResult submit(
        AccountFetchRequest,
        std::chrono::steady_clock::time_point) override {
        return AccountFetchSubmitResult::Stopped;
    }
    [[nodiscard]] std::vector<AccountFetchCompletion> drain_completions(
        std::chrono::steady_clock::time_point,
        std::size_t) override {
        return {};
    }
    void cancel(AccountFetchAttemptId) override {}
};

class GatewayIngressThrottlePipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        consumption_store_ =
            std::make_unique<InMemoryAdmissionConsumptionStore>(
                AdmissionConsumptionOptions{
                    .key_prefix = "/realmmesh/admission/throttle-test",
                    .reservation_ttl = 10s,
                    .digest_key = parse_admission_consumption_digest_key(
                        consumption_digest_key_hex)});
        admission_ = std::make_unique<GatewayAdmission>(
            common::IdentityTokenCodec(
                common::parse_identity_seed_hex(identity_seed_hex),
                "login-verify-v1"),
            "realmmesh/login-verify",
            common::AdmissionGrantVerifier(
                {{.kid = "admission-grant-v1",
                  .public_key = common::ed25519_public_key_from_seed(
                      common::parse_identity_seed_hex(grant_seed_hex))}},
                {.issuer = "realmmesh/queue",
                 .deployment_id = "development",
                 .grant_window = 300s}),
            *consumption_store_);

        GatewayLoginConfig config{
            .conn_capacity = 8,
            .fetch_capacity = 4,
            .fetch_retry_base = 100ms,
            .fetch_retry_max = 2,
            .handoff_grace = 5s,
        };
        // 收紧入口守卫:突发 1 枚、连续 2 次超限即判 sustained abuse。
        config.credential_ingress.source_rate_per_second = 1;
        config.credential_ingress.source_burst = 1;
        config.credential_ingress.source_throttle_close_after = 2;

        pipeline_.emplace(GatewayLoginPipeline::create(
            std::move(config),
            common::parse_ticket_key_hex(ticket_key_hex),
            *admission_,
            "gateway-throttle-01",
            transport_,
            fetch_));
    }

    void open(EdgeSessionId session_id) {
        transport_.push_event({
            .kind = GatewayEventKind::SessionOpened,
            .session_id = session_id,
            .source = "127.0.0.1",
        });
    }

    void attach(EdgeSessionId session_id, std::uint64_t request_id) {
        transport_.push_event({
            .kind = GatewayEventKind::MessageReceived,
            .session_id = session_id,
            .established = false,
            .payload = attach_payload(request_id),
            .source = "127.0.0.1",
        });
    }

    /// 同一 steady/system 时刻推进:桶不随时间补充,断言才是确定性的。
    void advance() {
        static_cast<void>(pipeline_->advance({
            .now = std::chrono::steady_clock::time_point{
                std::chrono::seconds{10}},
            .verification_now = std::chrono::system_clock::time_point{
                std::chrono::seconds{1'700'000'000}},
            .discovered_realm = std::nullopt,
        }));
    }

    [[nodiscard]] std::optional<PrimaryTransportCommand> last_command(
        PrimaryTransportCommandKind kind,
        EdgeSessionId session_id) const {
        std::optional<PrimaryTransportCommand> found;
        for (const auto& command : transport_.owned_commands()) {
            if (command.kind == kind && command.session_id == session_id) {
                found = command;
            }
        }
        return found;
    }

    [[nodiscard]] bool has_command(
        PrimaryTransportCommandKind kind,
        EdgeSessionId session_id) const {
        return last_command(kind, session_id).has_value();
    }

    std::unique_ptr<InMemoryAdmissionConsumptionStore> consumption_store_;
    std::unique_ptr<GatewayAdmission> admission_;
    InMemoryGatewayPrimaryTransport transport_;
    UnusedFetchPort fetch_;
    std::optional<GatewayLoginPipeline> pipeline_;
};

/// 线契约:正常限流回 429 + retry_after_seconds(客户端可带窗口重试同一
/// 凭据链),连续滥用则直接关闭连接、不给任何错误细节。
TEST_F(
    GatewayIngressThrottlePipelineTest,
    ThrottleIsRetryableErrorAndAbuseClosesConnection) {
    // 会话 1 用掉桶里唯一一枚令牌:凭据是垃圾,后续按凭据无效拒——但入口
    // 守卫这一关是放行的(限量已被消耗)。
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, 11);
    advance();
    EXPECT_TRUE(has_command(PrimaryTransportCommandKind::Decline, EdgeSessionId{1}));
    EXPECT_FALSE(
        has_command(PrimaryTransportCommandKind::Close, EdgeSessionId{1}));

    // 会话 2:桶已空 → 429 + Retry-After ≥ 1s。
    open(EdgeSessionId{2});
    attach(EdgeSessionId{2}, 12);
    advance();
    const auto throttled =
        last_command(PrimaryTransportCommandKind::Decline, EdgeSessionId{2});
    ASSERT_TRUE(throttled.has_value());
    const auto error = common::decode_edge_error(throttled->payload);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(
        error->code(),
        static_cast<std::uint32_t>(common::edge_error_throttled));
    EXPECT_GE(error->retry_after_seconds(), 1U);
    EXPECT_FALSE(
        has_command(PrimaryTransportCommandKind::Close, EdgeSessionId{2}));

    // 会话 3:同来源再次超限 → sustained abuse → 关闭,没有 429 回包。
    open(EdgeSessionId{3});
    attach(EdgeSessionId{3}, 13);
    advance();
    EXPECT_TRUE(has_command(PrimaryTransportCommandKind::Close, EdgeSessionId{3}));
    EXPECT_FALSE(
        has_command(PrimaryTransportCommandKind::Decline, EdgeSessionId{3}));
}

}  // namespace
}  // namespace realm::game::gateway
