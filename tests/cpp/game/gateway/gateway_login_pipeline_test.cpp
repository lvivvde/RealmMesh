#include "realmmesh/game/gateway/gateway_login_pipeline.hpp"

#include "realmmesh/game/common/admission_grant.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/admission_consumption_store.hpp"
#include "realmmesh/game/gateway/gateway_admission.hpp"
#include "realmmesh/observability/metrics_registry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace realm::game::gateway {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view identity_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view queue_seed_hex =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
constexpr std::string_view ticket_key_hex =
    "0102030405060708090a0b0c0d0e0f10"
    "1112131415161718191a1b1c1d1e1f20";
constexpr std::string_view consumption_digest_key_hex =
    "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7";
constexpr std::string_view jti_a = "000102030405060708090a0b0c0d0e0f";
constexpr std::string_view jti_b = "101112131415161718191a1b1c1d1e1f";
constexpr std::string_view jti_c = "202122232425262728292a2b2c2d2e2f";

const auto system_origin =
    std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};
const auto steady_origin =
    std::chrono::steady_clock::time_point{std::chrono::seconds{10}};

class GatewayLoginPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        identity_codec_.emplace(
            common::parse_identity_seed_hex(identity_seed_hex),
            "login-verify-v1");
    }

    [[nodiscard]] GatewayLoginConfig config(
        std::uint64_t conn_capacity = 4,
        std::uint64_t fetch_capacity = 2) const {
        return {
            .conn_capacity = conn_capacity,
            .fetch_capacity = fetch_capacity,
            .fetch_retry_base = 100ms,
            .fetch_retry_max = 2,
            .handoff_grace = 5s,
            .static_realm = RealmEndpoint{"static.realm", 7100},
        };
    }

    void create(GatewayLoginConfig pipeline_config) {
        create_admission(std::move(pipeline_config));
    }

    [[nodiscard]] std::string identity_token(
        std::string_view jti, std::uint64_t account_id = 42) const {
        return identity_codec_->issue(
            common::IdentityClaims{
                .issuer = "realmmesh/login-verify",
                .account_id = account_id,
                .jti = std::string(jti),
                .issued_at = system_origin,
                .expires_at = system_origin + 30min,
            });
    }

    [[nodiscard]] common::AdmissionGrantIssuer grant_issuer() const {
        return common::AdmissionGrantIssuer(
            {.kid = "grant-v1",
             .seed = common::parse_identity_seed_hex(queue_seed_hex)},
            {.issuer = "realmmesh/queue",
             .deployment_id = "prod-a",
             .grant_window = 5min});
    }

    [[nodiscard]] common::AdmissionGrantVerifier grant_verifier() const {
        const auto seed = common::parse_identity_seed_hex(queue_seed_hex);
        return common::AdmissionGrantVerifier(
            {{.kid = "grant-v1",
              .public_key = common::ed25519_public_key_from_seed(seed)}},
            {.issuer = "realmmesh/queue",
             .deployment_id = "prod-a",
             .grant_window = 5min});
    }

    [[nodiscard]] std::string grant_token(
        std::string_view identity_jti,
        std::string_view grant_jti = jti_c) const {
        return grant_issuer().issue(common::AdmissionGrantIssue{
            .grant_jti = std::string{grant_jti},
            .identity_jti = std::string{identity_jti},
            .queue_number = 7,
            .released_at = system_origin,
            .issued_at = system_origin,
            .identity_expires_at = system_origin + 30min,
        });
    }

    [[nodiscard]] AdmissionConsumptionOptions consumption_options() const {
        return {
            .key_prefix = "/realmmesh/admission/test",
            .reservation_ttl = 10s,
            .digest_key = parse_admission_consumption_digest_key(
                consumption_digest_key_hex),
        };
    }

    [[nodiscard]] GatewayAdmission make_admission(
        AdmissionConsumptionStore& store) const {
        return GatewayAdmission(
            common::IdentityTokenCodec(
                common::parse_identity_seed_hex(identity_seed_hex),
                "login-verify-v1"),
            "realmmesh/login-verify",
            grant_verifier(),
            store);
    }

    void create_admission(GatewayLoginConfig pipeline_config) {
        consumption_store_ =
            std::make_unique<InMemoryAdmissionConsumptionStore>(
                consumption_options());
        admission_ = std::make_unique<GatewayAdmission>(
            make_admission(*consumption_store_));
        pipeline_.emplace(GatewayLoginPipeline::create(
            std::move(pipeline_config),
            common::parse_ticket_key_hex(ticket_key_hex),
            *admission_,
            "gateway-a",
            transport_,
            fetch_,
            nullptr,
            &metrics_));
    }

    void open(EdgeSessionId session_id) {
        transport_.push_event({
            .kind = GatewayEventKind::SessionOpened,
            .session_id = session_id,
            .source = "127.0.0.1",
        });
    }

    void attach(
        EdgeSessionId session_id,
        std::string_view jti,
        std::uint64_t request_id = 1) {
        attach_with_tokens(
            transport_,
            session_id,
            identity_token(jti),
            grant_token(jti),
            request_id);
    }

    static void attach_with_tokens(
        InMemoryGatewayPrimaryTransport& transport,
        EdgeSessionId session_id,
        std::string_view identity,
        std::string_view number,
        std::uint64_t request_id = 1) {
        common::EdgeAttach attach;
        attach.set_identity_token(identity);
        attach.set_admission_grant(number);
        transport.push_event({
            .kind = GatewayEventKind::MessageReceived,
            .session_id = session_id,
            .established = false,
            .payload = common::encode(attach, request_id),
        });
    }

    void close(EdgeSessionId session_id, bool established = false) {
        transport_.push_event({
            .kind = GatewayEventKind::SessionClosed,
            .session_id = session_id,
            .established = established,
        });
    }

    [[nodiscard]] GatewayLoginAdvanceResult advance(
        std::chrono::milliseconds steady_offset = 0ms,
        std::chrono::milliseconds system_offset = 0ms,
        std::optional<RealmEndpoint> discovered = std::nullopt) {
        return pipeline_->advance({
            .now = steady_origin + steady_offset,
            .verification_now = system_origin + system_offset,
            .discovered_realm = std::move(discovered),
        });
    }

    [[nodiscard]] std::size_t command_count(
        PrimaryTransportCommandKind kind) const {
        return static_cast<std::size_t>(std::ranges::count_if(
            transport_.owned_commands(), [kind](const auto& command) {
                return command.kind == kind;
            }));
    }

    [[nodiscard]] const PrimaryTransportCommand& last_command(
        PrimaryTransportCommandKind kind) const {
        const auto found = std::ranges::find_if(
            transport_.owned_commands() | std::views::reverse,
            [kind](const auto& command) {
                return command.kind == kind;
            });
        EXPECT_NE(
            found,
            std::ranges::end(
                transport_.owned_commands() | std::views::reverse));
        return *found;
    }

    InMemoryGatewayPrimaryTransport transport_;
    ScriptedAccountFetchPort fetch_;
    observability::MetricsRegistry metrics_;
    std::optional<common::IdentityTokenCodec> identity_codec_;
    std::unique_ptr<InMemoryAdmissionConsumptionStore> consumption_store_;
    std::unique_ptr<GatewayAdmission> admission_;
    std::optional<GatewayLoginPipeline> pipeline_;
};

TEST_F(GatewayLoginPipelineTest, CreationValidatesConfigurationAndInstance) {
    auto invalid_config = config();
    invalid_config.fetch_capacity = 0;
    InMemoryAdmissionConsumptionStore store(consumption_options());
    auto admission = make_admission(store);
    EXPECT_THROW(
        static_cast<void>(GatewayLoginPipeline::create(
            invalid_config,
            common::parse_ticket_key_hex(ticket_key_hex),
            admission,
            "gateway-a",
            transport_,
            fetch_)),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(GatewayLoginPipeline::create(
            config(),
            common::parse_ticket_key_hex(ticket_key_hex),
            admission,
            "gateway with spaces",
            transport_,
            fetch_)),
        std::invalid_argument);
}

TEST_F(
    GatewayLoginPipelineTest,
    AcceptFullFullQueuedKeepsOneReservationAndDoesNotOversubscribeFetch) {
    create(config(3, 1));
    transport_.script_result(
        PrimaryTransportCommandKind::Accept,
        {PrimaryTransportResult::Full,
         PrimaryTransportResult::Full,
         PrimaryTransportResult::Queued});

    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    auto result = advance();
    EXPECT_EQ(result.local_budget.fetch_free, 0U);
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Accept), 0U);
    EXPECT_NE(
        metrics_.render().find("edge_sessions{stage=\"pending\"} 1\n"),
        std::string::npos);
    EXPECT_EQ(metrics_.render().find("accept_pending"), std::string::npos);

    open(EdgeSessionId{2});
    attach(EdgeSessionId{2}, jti_b, 2);
    result = advance(1ms, 1ms);
    EXPECT_EQ(result.local_budget.fetch_free, 0U);
    ASSERT_EQ(command_count(PrimaryTransportCommandKind::Decline), 1U);
    const auto rejected = common::decode_edge_error(
        last_command(PrimaryTransportCommandKind::Decline).payload);
    ASSERT_TRUE(rejected.has_value());
    EXPECT_EQ(rejected->code(), common::edge_error_attach_out_of_budget);

    result = advance(2ms, 2ms);
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Accept), 1U);
    EXPECT_EQ(result.local_budget.fetch_free, 0U);
    EXPECT_TRUE(fetch_.submitted_requests().empty());

    static_cast<void>(advance(3ms, 3ms));
    ASSERT_EQ(fetch_.submitted_requests().size(), 1U);
    EXPECT_EQ(fetch_.submitted_requests()[0].session_id, EdgeSessionId{1});
}

TEST_F(
    GatewayLoginPipelineTest,
    ReservedAndCommittedJtiRejectConcurrentAndLaterReplay) {
    create(config(3, 2));
    transport_.script_result(
        PrimaryTransportCommandKind::Accept,
        {PrimaryTransportResult::Full,
         PrimaryTransportResult::Full,
         PrimaryTransportResult::Queued});
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    static_cast<void>(advance());

    open(EdgeSessionId{2});
    attach(EdgeSessionId{2}, jti_a, 2);
    static_cast<void>(advance(1ms, 1ms));
    ASSERT_EQ(command_count(PrimaryTransportCommandKind::Decline), 1U);
    auto rejected = common::decode_edge_error(
        last_command(PrimaryTransportCommandKind::Decline).payload);
    ASSERT_TRUE(rejected.has_value());
    // 同一 identity_jti 的并发准入是「别人正在处理」:对外可重试(1005),
    // 既不消耗凭据也不计入重放拒绝——凭据此刻仍然有效(ADR-0009)。
    EXPECT_EQ(rejected->code(), common::edge_error_admission_in_progress);
    EXPECT_EQ(
        metrics_.render().find("edge_jti_replay_rejected_total 1\n"),
        std::string::npos);

    static_cast<void>(advance(2ms, 2ms));
    ASSERT_EQ(command_count(PrimaryTransportCommandKind::Accept), 1U);
    open(EdgeSessionId{3});
    attach(EdgeSessionId{3}, jti_a, 3);
    static_cast<void>(advance(3ms, 3ms));
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Decline), 2U);
    // 已提交消费之后的重放是终态:对外统一收敛为凭据无效,内部留下重放
    // 拒绝计数以便与跨身份拼接区分。
    const auto replayed = common::decode_edge_error(
        last_command(PrimaryTransportCommandKind::Decline).payload);
    ASSERT_TRUE(replayed.has_value());
    EXPECT_EQ(replayed->code(), common::edge_error_invalid_credentials);
    EXPECT_NE(
        metrics_.render().find("edge_jti_replay_rejected_total 1\n"),
        std::string::npos);
}

TEST_F(
    GatewayLoginPipelineTest,
    AdmissionRejectsCrossIdentityCredentialSplice) {
    create(config(1, 1));
    const auto alice_identity = identity_token(jti_a, 42);
    const auto bob_identity = identity_token(jti_b, 84);
    const auto grant_obtained_by_bob = grant_token(jti_b);
    ASSERT_TRUE(
        identity_codec_
            ->validate(bob_identity, "realmmesh/login-verify", system_origin)
            .has_value());

    open(EdgeSessionId{1});
    attach_with_tokens(
        transport_,
        EdgeSessionId{1},
        alice_identity,
        grant_obtained_by_bob);
    static_cast<void>(advance());

    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Accept), 0U);
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Decline), 1U);
}

TEST_F(
    GatewayLoginPipelineTest,
    AdmissionFullAndPrecommitCloseReleaseWithoutBurningCredentials) {
    create_admission(config(2, 1));
    transport_.script_result(
        PrimaryTransportCommandKind::Accept,
        {PrimaryTransportResult::Full});
    const auto identity = identity_token(jti_a);
    const auto grant = grant_token(jti_a);

    open(EdgeSessionId{1});
    attach_with_tokens(
        transport_, EdgeSessionId{1}, identity, grant);
    auto result = advance();
    EXPECT_EQ(result.local_budget.fetch_free, 0U);
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Accept), 0U);

    close(EdgeSessionId{1});
    result = advance(1ms, 1ms);
    EXPECT_EQ(result.local_budget.fetch_free, 1U);

    open(EdgeSessionId{2});
    attach_with_tokens(
        transport_, EdgeSessionId{2}, identity, grant, 2);
    result = advance(2ms, 2ms);
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Accept), 1U);
    EXPECT_EQ(result.local_budget.fetch_free, 0U);
}

TEST_F(
    GatewayLoginPipelineTest,
    AdmissionSharedStoreCommitsAcrossPipelineInstancesAtMostOnce) {
    InMemoryAdmissionConsumptionStore store(consumption_options());
    auto first_admission = make_admission(store);
    auto second_admission = make_admission(store);
    InMemoryGatewayPrimaryTransport first_transport;
    InMemoryGatewayPrimaryTransport second_transport;
    ScriptedAccountFetchPort first_fetch;
    ScriptedAccountFetchPort second_fetch;
    observability::MetricsRegistry first_metrics;
    observability::MetricsRegistry second_metrics;
    auto first = GatewayLoginPipeline::create(
        config(1, 1),
        common::parse_ticket_key_hex(ticket_key_hex),
        first_admission,
        "gateway-a",
        first_transport,
        first_fetch,
        nullptr,
        &first_metrics);
    auto second = GatewayLoginPipeline::create(
        config(1, 1),
        common::parse_ticket_key_hex(ticket_key_hex),
        second_admission,
        "gateway-b",
        second_transport,
        second_fetch,
        nullptr,
        &second_metrics);
    const auto identity = identity_token(jti_a);
    const auto grant = grant_token(jti_a);
    for (auto* transport : {&first_transport, &second_transport}) {
        transport->push_event({
            .kind = GatewayEventKind::SessionOpened,
            .session_id = EdgeSessionId{1},
            .source = "127.0.0.1",
        });
        attach_with_tokens(
            *transport, EdgeSessionId{1}, identity, grant);
    }

    const GatewayLoginFrame input{
        .now = steady_origin,
        .verification_now = system_origin,
    };
    static_cast<void>(first.advance(input));
    static_cast<void>(second.advance(input));

    ASSERT_EQ(first_transport.owned_commands().size(), 1U);
    EXPECT_EQ(
        first_transport.owned_commands()[0].kind,
        PrimaryTransportCommandKind::Accept);
    ASSERT_EQ(second_transport.owned_commands().size(), 1U);
    EXPECT_EQ(
        second_transport.owned_commands()[0].kind,
        PrimaryTransportCommandKind::Decline);
    EXPECT_NE(
        second_metrics.render().find("edge_jti_replay_rejected_total 1\n"),
        std::string::npos);
}

TEST_F(
    GatewayLoginPipelineTest,
    AdmissionRejectsCrossIdentitySpliceBeforeTransportAccept) {
    create_admission(config(1, 1));
    open(EdgeSessionId{1});
    attach_with_tokens(
        transport_,
        EdgeSessionId{1},
        identity_token(jti_a, 42),
        grant_token(jti_b));

    static_cast<void>(advance());

    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Accept), 0U);
    ASSERT_EQ(command_count(PrimaryTransportCommandKind::Decline), 1U);
    const auto error = common::decode_edge_error(
        last_command(PrimaryTransportCommandKind::Decline).payload);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code(), common::edge_error_invalid_credentials);
}

TEST_F(
    GatewayLoginPipelineTest,
    AdmissionStoreOutageFailsNewAttachClosedWithoutStoppingExistingFetch) {
    create_admission(config(2, 2));
    open(EdgeSessionId{1});
    attach_with_tokens(
        transport_,
        EdgeSessionId{1},
        identity_token(jti_a),
        grant_token(jti_a));
    static_cast<void>(advance());
    static_cast<void>(advance(1ms, 1ms));
    ASSERT_EQ(fetch_.submitted_requests().size(), 1U);
    const auto active = fetch_.submitted_requests()[0].attempt_id;

    consumption_store_->set_available(false);
    open(EdgeSessionId{2});
    attach_with_tokens(
        transport_,
        EdgeSessionId{2},
        identity_token(jti_b),
        grant_token(jti_b, jti_a),
        2);
    const auto result = advance(2ms, 2ms);

    EXPECT_EQ(result.health, GatewayPipelineHealth::Healthy);
    EXPECT_FALSE(result.local_budget.available);
    EXPECT_FALSE(fetch_.was_cancelled(active));
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Decline), 1U);
}

class AmbiguousPipelineCommitStore final : public AdmissionConsumptionStore {
public:
    AdmissionReserveResult reserve(
        const AdmissionReserveRequest& request) override {
        return {
            .status = AdmissionReserveStatus::Reserved,
            .reservation = AdmissionReservation{
                .identity_jti = request.identity_jti,
                .grant_jti = request.grant_jti,
                .owner = request.owner,
                .fencing = 1,
                .lease_expires_at = request.now + 10s,
                .consume_until = request.consume_until,
            },
        };
    }

    AdmissionMutationStatus commit(
        const AdmissionReservation&,
        std::chrono::system_clock::time_point) override {
        ++commit_calls;
        return AdmissionMutationStatus::Unavailable;
    }

    AdmissionMutationStatus release(
        const AdmissionReservation&,
        std::chrono::system_clock::time_point) override {
        ++release_calls;
        return AdmissionMutationStatus::Applied;
    }

    bool available() const noexcept override { return false; }

    int commit_calls{0};
    int release_calls{0};
};

TEST_F(
    GatewayLoginPipelineTest,
    AdmissionAmbiguousCommitClosesWithoutFetchOrRelease) {
    AmbiguousPipelineCommitStore store;
    auto admission = make_admission(store);
    pipeline_.emplace(GatewayLoginPipeline::create(
        config(1, 1),
        common::parse_ticket_key_hex(ticket_key_hex),
        admission,
        "gateway-a",
        transport_,
        fetch_,
        nullptr,
        &metrics_));
    open(EdgeSessionId{1});
    attach_with_tokens(
        transport_,
        EdgeSessionId{1},
        identity_token(jti_a),
        grant_token(jti_a));

    const auto result = advance();
    EXPECT_EQ(result.health, GatewayPipelineHealth::Healthy);
    EXPECT_FALSE(result.local_budget.available);
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Accept), 1U);
    EXPECT_TRUE(fetch_.submitted_requests().empty());
    EXPECT_EQ(store.commit_calls, 1);
    EXPECT_EQ(store.release_calls, 0);

    static_cast<void>(advance(1ms, 1ms));
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Close), 1U);
    EXPECT_EQ(store.release_calls, 0);
    pipeline_.reset();
}

TEST_F(
    GatewayLoginPipelineTest,
    CloseBeforeAcceptCommitReleasesJtiAndFetchReservation) {
    create(config(2, 1));
    transport_.script_result(
        PrimaryTransportCommandKind::Accept, {PrimaryTransportResult::Full});
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    auto result = advance();
    EXPECT_EQ(result.local_budget.fetch_free, 0U);

    close(EdgeSessionId{1});
    result = advance(1ms, 1ms);
    EXPECT_EQ(result.local_budget.fetch_free, 1U);
    EXPECT_EQ(result.local_budget.conn_free, 2U);

    open(EdgeSessionId{2});
    attach(EdgeSessionId{2}, jti_a);
    result = advance(2ms, 2ms);
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Accept), 1U);
    EXPECT_EQ(result.local_budget.fetch_free, 0U);
    EXPECT_NE(
        metrics_.render().find("edge_jti_replay_rejected_total 0\n"),
        std::string::npos);
}

TEST_F(
    GatewayLoginPipelineTest,
    SameFrameCloseWinsOverFetchSuccessAndLateDuplicateCompletions) {
    create(config(1, 1));
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    static_cast<void>(advance());
    static_cast<void>(advance(1ms, 1ms));
    ASSERT_EQ(fetch_.submitted_requests().size(), 1U);
    const auto attempt = fetch_.submitted_requests()[0].attempt_id;

    close(EdgeSessionId{1}, true);
    fetch_.push_completion({attempt, true, 5ms});
    fetch_.push_completion({attempt, true, 6ms});
    fetch_.push_completion({AccountFetchAttemptId{999}, true, 7ms});
    const auto result = advance(2ms, 2ms);

    EXPECT_TRUE(fetch_.was_cancelled(attempt));
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::SendHandoff), 0U);
    EXPECT_EQ(result.local_budget.conn_free, 1U);
    EXPECT_EQ(result.local_budget.fetch_free, 1U);
}

TEST_F(GatewayLoginPipelineTest, DestructionCancelsAcceptedFetchWork) {
    create(config(1, 1));
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    static_cast<void>(advance());
    static_cast<void>(advance(1ms, 1ms));
    ASSERT_EQ(fetch_.submitted_requests().size(), 1U);
    const auto attempt = fetch_.submitted_requests()[0].attempt_id;

    pipeline_.reset();

    EXPECT_TRUE(fetch_.was_cancelled(attempt));
}

TEST_F(
    GatewayLoginPipelineTest,
    FetchSubmitFullDoesNotConsumeRetryAndFailureUsesBackoff) {
    auto pipeline_config = config(1, 1);
    pipeline_config.fetch_retry_base = 100ms;
    create(std::move(pipeline_config));
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    static_cast<void>(advance());
    fetch_.script_submit_results(
        {AccountFetchSubmitResult::Full,
         AccountFetchSubmitResult::Submitted,
         AccountFetchSubmitResult::Submitted});

    static_cast<void>(advance(1ms, 1ms));
    EXPECT_TRUE(fetch_.submitted_requests().empty());
    auto rendered = metrics_.render();
    EXPECT_NE(
        rendered.find("edge_fetch_submit_backpressure_total 1\n"),
        std::string::npos);
    EXPECT_NE(rendered.find("edge_fetch_retry_total 0\n"), std::string::npos);

    static_cast<void>(advance(2ms, 2ms));
    ASSERT_EQ(fetch_.submitted_requests().size(), 1U);
    fetch_.push_completion(
        {fetch_.submitted_requests()[0].attempt_id, false, 10ms});
    static_cast<void>(advance(3ms, 3ms));
    static_cast<void>(advance(102ms, 102ms));
    EXPECT_EQ(fetch_.submitted_requests().size(), 1U);
    static_cast<void>(advance(103ms, 103ms));
    ASSERT_EQ(fetch_.submitted_requests().size(), 2U);
    EXPECT_NE(
        metrics_.render().find("edge_fetch_retry_total 1\n"),
        std::string::npos);
}

TEST_F(
    GatewayLoginPipelineTest,
    HandoffFullKeepsFetchingAndQueuedAttemptUsesFreshTicketAndDynamicEndpoint) {
    create(config(1, 1));
    transport_.script_result(
        PrimaryTransportCommandKind::SendHandoff,
        {PrimaryTransportResult::Full, PrimaryTransportResult::Queued});
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    static_cast<void>(advance());
    static_cast<void>(advance(1ms, 1ms));
    ASSERT_EQ(fetch_.submitted_requests().size(), 1U);
    fetch_.push_completion({
        .attempt_id = fetch_.submitted_requests()[0].attempt_id,
        .ok = true,
        .duration = 20ms,
        .status = AccountFetchStatus::Succeeded,
    });

    static_cast<void>(advance(2ms, 2ms, RealmEndpoint{"first.dynamic", 7200}));
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::SendHandoff), 0U);
    auto rendered = metrics_.render();
    EXPECT_NE(
        rendered.find("edge_sessions{stage=\"fetching\"} 1\n"),
        std::string::npos);
    EXPECT_NE(
        rendered.find("edge_budget{kind=\"fetch_free\"} 1\n"),
        std::string::npos);

    static_cast<void>(
        advance(30s, 30s, RealmEndpoint{"current.dynamic", 7300}));
    ASSERT_EQ(command_count(PrimaryTransportCommandKind::SendHandoff), 1U);
    const auto granted = common::decode_enter_realm_granted(
        last_command(PrimaryTransportCommandKind::SendHandoff).payload);
    ASSERT_TRUE(granted.has_value());
    ASSERT_EQ(granted->realm_endpoints_size(), 1);
    EXPECT_EQ(granted->realm_endpoints(0).address(), "current.dynamic");
    EXPECT_EQ(granted->realm_endpoints(0).port(), 7300U);

    common::SessionTicketCodec ticket_codec(
        common::parse_ticket_key_hex(ticket_key_hex));
    const auto claims = ticket_codec.validate(
        common::protobuf_bytes(granted->enter_realm_ticket()),
        common::TicketPurpose::EnterRealm,
        system_origin + 30s);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->expires_at, system_origin + 90s);
    EXPECT_EQ(claims->realm_id, 1U);
    EXPECT_NE(
        metrics_.render().find(
            "edge_fetch_result_total{result=\"succeeded\"} 1\n"),
        std::string::npos);
    EXPECT_NE(
        metrics_.render().find("edge_sessions{stage=\"handed_off\"} 1\n"),
        std::string::npos);
}

TEST_F(
    GatewayLoginPipelineTest,
    GraceCloseFullRetainsDeadlineStageAndConnectionUntilSessionClosed) {
    create(config(1, 1));
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    static_cast<void>(advance());
    static_cast<void>(advance(1ms, 1ms));
    fetch_.push_completion(
        {fetch_.submitted_requests()[0].attempt_id, true, 1ms});
    static_cast<void>(advance(2ms, 2ms));
    const auto granted = common::decode_enter_realm_granted(
        last_command(PrimaryTransportCommandKind::SendHandoff).payload);
    ASSERT_TRUE(granted.has_value());
    ASSERT_EQ(granted->realm_endpoints_size(), 1);
    EXPECT_EQ(granted->realm_endpoints(0).address(), "static.realm");
    EXPECT_EQ(granted->realm_endpoints(0).port(), 7100U);
    transport_.script_result(
        PrimaryTransportCommandKind::Close,
        {PrimaryTransportResult::Full, PrimaryTransportResult::Queued});

    auto result = advance(5'002ms, 5'002ms);
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Close), 0U);
    EXPECT_EQ(result.local_budget.conn_free, 0U);
    EXPECT_NE(
        metrics_.render().find("edge_sessions{stage=\"handed_off\"} 1\n"),
        std::string::npos);

    result = advance(5'003ms, 5'003ms);
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Close), 1U);
    EXPECT_EQ(result.local_budget.conn_free, 0U);

    close(EdgeSessionId{1}, true);
    result = advance(5'004ms, 5'004ms);
    EXPECT_EQ(result.local_budget.conn_free, 1U);
    EXPECT_NE(
        metrics_.render().find("edge_sessions{stage=\"handed_off\"} 0\n"),
        std::string::npos);
}

TEST_F(
    GatewayLoginPipelineTest,
    MissingEndpointCreatesCloseIntentWithoutIssuingHandoff) {
    auto pipeline_config = config(1, 1);
    pipeline_config.static_realm.reset();
    create(std::move(pipeline_config));
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    static_cast<void>(advance());
    static_cast<void>(advance(1ms, 1ms));
    fetch_.push_completion(
        {fetch_.submitted_requests()[0].attempt_id, true, 1ms});

    static_cast<void>(advance(2ms, 2ms));
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::SendHandoff), 0U);
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Close), 0U);
    const auto result = advance(3ms, 3ms);
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Close), 1U);
    EXPECT_EQ(result.local_budget.conn_free, 0U);
    EXPECT_EQ(result.local_budget.fetch_free, 1U);
}

TEST_F(
    GatewayLoginPipelineTest,
    ExhaustedFetchReleasesFetchCapacityAndDeclinesAsPlayerDataUnavailable) {
    auto pipeline_config = config(1, 1);
    pipeline_config.fetch_retry_max = 1;
    create(std::move(pipeline_config));
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    static_cast<void>(advance());
    static_cast<void>(advance(1ms, 1ms));
    fetch_.push_completion(
        {fetch_.submitted_requests()[0].attempt_id, false, 1ms});
    static_cast<void>(advance(2ms, 2ms));
    static_cast<void>(advance(102ms, 102ms));
    ASSERT_EQ(fetch_.submitted_requests().size(), 2U);
    fetch_.push_completion(
        {fetch_.submitted_requests()[1].attempt_id, false, 1ms});

    const auto result = advance(103ms, 103ms);
    // 重试耗尽不再静默断开:客户端须能把数据源故障与封禁区分开(#98)。
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::Close), 0U);
    ASSERT_EQ(command_count(PrimaryTransportCommandKind::Decline), 1U);
    const auto declined = common::decode_edge_error(
        last_command(PrimaryTransportCommandKind::Decline).payload);
    ASSERT_TRUE(declined.has_value());
    EXPECT_EQ(declined->code(), common::edge_error_player_data_unavailable);
    EXPECT_GT(declined->retry_after_seconds(), 0U);
    EXPECT_EQ(result.local_budget.fetch_free, 1U);
    EXPECT_EQ(result.local_budget.conn_free, 0U);
    EXPECT_NE(
        metrics_.render().find("edge_fetch_retry_total 1\n"),
        std::string::npos);
}

TEST_F(
    GatewayLoginPipelineTest,
    NotEligibleFetchDeclinesImmediatelyWithoutRetry) {
    create(config(1, 1));
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    static_cast<void>(advance());
    static_cast<void>(advance(1ms, 1ms));
    ASSERT_EQ(fetch_.submitted_requests().size(), 1U);
    fetch_.push_completion({
        .attempt_id = fetch_.submitted_requests()[0].attempt_id,
        .ok = false,
        .duration = 1ms,
        .status = AccountFetchStatus::NotEligible,
    });

    auto result = advance(2ms, 2ms);
    ASSERT_EQ(command_count(PrimaryTransportCommandKind::Decline), 1U);
    const auto declined = common::decode_edge_error(
        last_command(PrimaryTransportCommandKind::Decline).payload);
    ASSERT_TRUE(declined.has_value());
    EXPECT_EQ(declined->code(), common::edge_error_not_eligible);
    EXPECT_EQ(result.local_budget.fetch_free, 1U);

    // 封禁/白名单/无角色是确定性结果:退避时间过去也不再重拉。
    result = advance(10s, 10s);
    EXPECT_EQ(fetch_.submitted_requests().size(), 1U);
    EXPECT_NE(
        metrics_.render().find("edge_fetch_retry_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        metrics_.render().find(
            "edge_fetch_result_total{result=\"not_eligible\"} 1\n"),
        std::string::npos);
}

TEST_F(
    GatewayLoginPipelineTest,
    FetchTimeoutCancelsAttemptAndRetriesWithBackoff) {
    auto pipeline_config = config(1, 1);
    pipeline_config.fetch_timeout = 500ms;
    pipeline_config.fetch_retry_base = 100ms;
    create(std::move(pipeline_config));
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    static_cast<void>(advance());
    static_cast<void>(advance(1ms, 1ms));
    ASSERT_EQ(fetch_.submitted_requests().size(), 1U);
    const auto stuck = fetch_.submitted_requests()[0].attempt_id;

    static_cast<void>(advance(500ms, 500ms));
    EXPECT_FALSE(fetch_.was_cancelled(stuck));

    static_cast<void>(advance(501ms, 501ms));
    EXPECT_TRUE(fetch_.was_cancelled(stuck));
    EXPECT_NE(
        metrics_.render().find(
            "edge_fetch_result_total{result=\"timeout\"} 1\n"),
        std::string::npos);

    // 迟到的完成不得复活已超时的尝试。
    fetch_.push_completion({stuck, true, 600ms});
    static_cast<void>(advance(502ms, 502ms));
    EXPECT_EQ(command_count(PrimaryTransportCommandKind::SendHandoff), 0U);
    EXPECT_EQ(fetch_.submitted_requests().size(), 1U);

    static_cast<void>(advance(601ms, 601ms));
    ASSERT_EQ(fetch_.submitted_requests().size(), 2U);
    EXPECT_NE(
        metrics_.render().find("edge_fetch_retry_total 1\n"),
        std::string::npos);
}

TEST_F(
    GatewayLoginPipelineTest,
    StoppedPrimaryTransportMakesPipelineUnhealthyAndBudgetUnavailable) {
    create(config(1, 1));
    transport_.script_result(
        PrimaryTransportCommandKind::Accept, {PrimaryTransportResult::Stopped});
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);

    const auto result = advance();
    EXPECT_EQ(result.health, GatewayPipelineHealth::Unhealthy);
    EXPECT_FALSE(result.local_budget.available);
}

TEST_F(
    GatewayLoginPipelineTest,
    StoppedFetchPortMakesPipelineUnhealthyAndBudgetUnavailable) {
    create(config(1, 1));
    open(EdgeSessionId{1});
    attach(EdgeSessionId{1}, jti_a);
    static_cast<void>(advance());
    fetch_.stop();

    const auto result = advance(1ms, 1ms);
    EXPECT_EQ(result.health, GatewayPipelineHealth::Unhealthy);
    EXPECT_FALSE(result.local_budget.available);
}

}  // namespace
}  // namespace realm::game::gateway
