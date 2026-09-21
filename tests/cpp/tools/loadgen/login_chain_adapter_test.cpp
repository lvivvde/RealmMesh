#include "realmmesh/loadgen/login_chain_adapter.hpp"
#include "realmmesh/loadgen/login_chain_metrics.hpp"
#include "realmmesh/loadgen/loadgen.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <utility>

namespace realm::loadgen {
namespace {

using std::chrono::milliseconds;

class TestGatewaySession final : public client::GatewaySession {};
class TestRealmSession final : public client::RealmSession {};

template <typename T>
[[nodiscard]] client::PortValue<T> value_with(client::PortStatus status,
                                              T value = {}) {
    return {.status = std::move(status), .value = std::move(value)};
}

class ScriptedLoginTransport final : public client::LoginChainTransport {
public:
    client::PortValue<client::VerifyResult> verify(
        std::string_view,
        std::string_view,
        client::TimePoint) override {
        return value_with(verify_status, client::VerifyResult{"identity"});
    }

    client::PortValue<client::TicketResult> take_ticket(
        std::string_view,
        client::TimePoint) override {
        return value_with(ticket_status,
                          client::TicketResult{"number-token", 7});
    }

    client::PortValue<client::ProgressResult> poll_progress(
        client::TimePoint) override {
        return value_with(progress_status, client::ProgressResult{7, 1.0});
    }

    client::PortValue<client::TicketMeResult> ticket_me(
        std::string_view,
        client::TimePoint) override {
        client::TicketMeResult result;
        result.admission_grant = "grant";
        result.admission_grant_ttl = std::chrono::seconds{300};
        return value_with(ticket_me_status, std::move(result));
    }

    client::PortValue<std::unique_ptr<client::GatewaySession>>
    connect_gateway(
        std::span<const network::client::EndpointCandidate>,
        client::TimePoint) override {
        return value_with(
            gateway_status,
            gateway_status.ok
                ? std::unique_ptr<client::GatewaySession>{
                      std::make_unique<TestGatewaySession>()}
                : nullptr);
    }

    client::PortStatus attach(client::GatewaySession&,
                              std::string_view,
                              std::string_view,
                              client::TimePoint) override {
        return attach_status;
    }

    client::PortValue<client::HandoffResult> await_handoff(
        client::GatewaySession&,
        client::TimePoint) override {
        return value_with(handoff_status, client::HandoffResult{});
    }

    client::PortValue<std::unique_ptr<client::RealmSession>> connect_realm(
        std::span<const network::client::EndpointCandidate>,
        client::TimePoint) override {
        return value_with(
            client::PortStatus::success(),
            std::unique_ptr<client::RealmSession>{
                std::make_unique<TestRealmSession>()});
    }

    client::PortStatus enter_realm(client::RealmSession&,
                                   std::string_view,
                                   client::TimePoint) override {
        return client::PortStatus::success();
    }

    client::PortStatus verify_status{client::PortStatus::success()};
    client::PortStatus ticket_status{client::PortStatus::success()};
    client::PortStatus progress_status{client::PortStatus::success()};
    client::PortStatus ticket_me_status{client::PortStatus::success()};
    client::PortStatus gateway_status{client::PortStatus::success()};
    client::PortStatus attach_status{client::PortStatus::success()};
    client::PortStatus handoff_status{client::PortStatus::success()};
};

[[nodiscard]] LoginChainCounters counters_for(LoadgenReport& report) {
    return {report.verify, report.tickets, report.poll, report.attach,
            report.handoff};
}

[[nodiscard]] LoadgenLoginOptions valid_options(LoadgenLoginTarget target) {
    LoadgenLoginOptions options;
    options.target = target;
    options.account = "alice";
    options.credential = "secret";
    options.poll_interval = milliseconds{25};
    options.deadline = client::Clock::now() + std::chrono::seconds{5};
    options.endpoints.login_verify = {"verify.example", 7001};
    options.endpoints.queue = {"queue.example", 7002};
    options.endpoints.gateway = {"gateway.example", 7003};
    if (target == LoadgenLoginTarget::GatewaySoak) {
        options.hold_until = client::Clock::now() + std::chrono::seconds{2};
    }
    return options;
}

TEST(LoginChainAdapterTest, MapsEveryTargetToValidatedLoginRun) {
    const auto expect_target = [](LoadgenLoginTarget loadgen_target,
                                  client::LoginTarget client_target,
                                  client::PollingProfile polling) {
        auto adaptation = adapt_login_run(valid_options(loadgen_target));
        auto* adapted = std::get_if<AdaptedLoginRun>(&adaptation);
        ASSERT_NE(adapted, nullptr);
        EXPECT_EQ(adapted->run.target(), client_target);
        EXPECT_EQ(adapted->run.polling_profile(), polling);
        EXPECT_EQ(adapted->chain.pressure_poll_interval, milliseconds{25});
        EXPECT_FALSE(adapted->wire.verify_peer);
        EXPECT_TRUE(adapted->transport.reset_close_on_release);
    };

    expect_target(LoadgenLoginTarget::Verify, client::LoginTarget::Verify,
                  client::PollingProfile::ClientRealistic);
    expect_target(LoadgenLoginTarget::Tickets, client::LoginTarget::Tickets,
                  client::PollingProfile::ClientRealistic);
    expect_target(LoadgenLoginTarget::Poll, client::LoginTarget::Poll,
                  client::PollingProfile::Pressure);
    expect_target(LoadgenLoginTarget::Gateway, client::LoginTarget::Gateway,
                  client::PollingProfile::Pressure);
    expect_target(LoadgenLoginTarget::GatewaySoak,
                  client::LoginTarget::GatewaySoak,
                  client::PollingProfile::Pressure);
    expect_target(LoadgenLoginTarget::Full, client::LoginTarget::Full,
                  client::PollingProfile::Pressure);
}

TEST(LoginChainAdapterTest, RejectsInvalidTargetSpecificConfiguration) {
    auto missing_hold = valid_options(LoadgenLoginTarget::GatewaySoak);
    missing_hold.hold_until.reset();
    EXPECT_EQ(std::get<LoginRunConfigError>(adapt_login_run(missing_hold)),
              LoginRunConfigError::MissingSoakHorizon);

    auto unexpected_hold = valid_options(LoadgenLoginTarget::Poll);
    unexpected_hold.hold_until = client::Clock::now() +
        std::chrono::seconds{1};
    EXPECT_EQ(std::get<LoginRunConfigError>(adapt_login_run(unexpected_hold)),
              LoginRunConfigError::UnexpectedSoakHorizon);

    auto past_hold = valid_options(LoadgenLoginTarget::GatewaySoak);
    past_hold.hold_until = client::Clock::now() - milliseconds{1};
    EXPECT_EQ(std::get<LoginRunConfigError>(adapt_login_run(past_hold)),
              LoginRunConfigError::SoakHorizonInPast);

    auto bad_interval = valid_options(LoadgenLoginTarget::Poll);
    bad_interval.poll_interval = milliseconds{0};
    EXPECT_EQ(std::get<LoginRunConfigError>(adapt_login_run(bad_interval)),
              LoginRunConfigError::NonPositivePollInterval);

    auto missing_gateway = valid_options(LoadgenLoginTarget::Gateway);
    missing_gateway.endpoints.gateway.port = 0;
    EXPECT_EQ(std::get<LoginRunConfigError>(adapt_login_run(missing_gateway)),
              LoginRunConfigError::MissingGatewayEndpoint);
}

TEST(LoginChainAdapterTest, PollingProfileChangesPacingOnly) {
    auto realistic_options = valid_options(LoadgenLoginTarget::Gateway);
    realistic_options.polling = LoadgenPollingProfile::ClientRealistic;
    auto pressure_options = realistic_options;
    pressure_options.polling = LoadgenPollingProfile::Pressure;

    auto realistic_adaptation = adapt_login_run(realistic_options);
    auto pressure_adaptation = adapt_login_run(pressure_options);
    const auto* realistic =
        std::get_if<AdaptedLoginRun>(&realistic_adaptation);
    const auto* pressure = std::get_if<AdaptedLoginRun>(&pressure_adaptation);
    ASSERT_NE(realistic, nullptr);
    ASSERT_NE(pressure, nullptr);
    EXPECT_EQ(realistic->run.target(), pressure->run.target());
    EXPECT_EQ(realistic->run.account(), pressure->run.account());
    EXPECT_EQ(realistic->run.credential(), pressure->run.credential());
    EXPECT_EQ(realistic->run.deadline(), pressure->run.deadline());
    EXPECT_EQ(realistic->run.polling_profile(),
              client::PollingProfile::ClientRealistic);
    EXPECT_EQ(pressure->run.polling_profile(),
              client::PollingProfile::Pressure);
}

TEST(LoginChainMetricsTest, MapsOperationsToExistingPhaseCounters) {
    ScriptedLoginTransport inner;
    inner.ticket_status = client::PortStatus::error(
        client::ChainFailure::TicketRejected, "network", false,
        client::PortFailureCategory::Transport);
    inner.progress_status = client::PortStatus::error(
        client::ChainFailure::ProgressFailed, "bad progress");
    inner.ticket_me_status = client::PortStatus::error(
        client::ChainFailure::ProgressFailed, "network", false,
        client::PortFailureCategory::Transport);
    inner.attach_status = client::PortStatus::error(
        client::ChainFailure::AttachRejected, "late", false,
        client::PortFailureCategory::Timeout);
    inner.handoff_status = client::PortStatus::error(
        client::ChainFailure::HandoffRejected, "bad frame");

    LoadgenReport report;
    MetricsLoginChainTransport measured(inner, counters_for(report));
    const auto deadline = client::Clock::now() + std::chrono::seconds{1};

    EXPECT_TRUE(measured.verify("alice", "secret", deadline).status.ok);
    EXPECT_FALSE(measured.take_ticket("identity", deadline).status.ok);
    EXPECT_FALSE(measured.poll_progress(deadline).status.ok);
    EXPECT_FALSE(measured.ticket_me("number", deadline).status.ok);
    auto gateway = measured.connect_gateway({}, deadline);
    ASSERT_TRUE(gateway.status.ok);
    ASSERT_NE(gateway.value, nullptr);
    EXPECT_FALSE(
        measured.attach(*gateway.value, "identity", "grant", deadline).ok);
    EXPECT_FALSE(measured.await_handoff(*gateway.value, deadline).status.ok);

    EXPECT_EQ(report.verify.attempts, 1U);
    EXPECT_EQ(report.verify.failures, 0U);
    EXPECT_EQ(report.tickets.by_kind.at(FailureKind::ConnectionError), 1U);
    EXPECT_EQ(report.poll.by_kind.at(FailureKind::PollFailed), 1U);
    EXPECT_EQ(report.poll.by_kind.at(FailureKind::ConnectionError), 1U);
    EXPECT_EQ(report.attach.by_kind.at(FailureKind::AttachTimeout), 1U);
    EXPECT_EQ(report.handoff.by_kind.at(FailureKind::HandoffRejected), 1U);
}

TEST(LoginChainMetricsTest, PreservesGatewayDialAttachBucketing) {
    const auto deadline = client::Clock::now() + std::chrono::seconds{1};
    {
        ScriptedLoginTransport inner;
        LoadgenReport report;
        MetricsLoginChainTransport measured(inner, counters_for(report));
        auto gateway = measured.connect_gateway({}, deadline);
        ASSERT_TRUE(gateway.status.ok);
        EXPECT_EQ(report.attach.attempts, 0U);
        ASSERT_NE(gateway.value, nullptr);
        EXPECT_TRUE(
            measured.attach(*gateway.value, "identity", "grant", deadline).ok);
        EXPECT_EQ(report.attach.attempts, 1U);
        EXPECT_EQ(report.attach.failures, 0U);
    }
    {
        ScriptedLoginTransport inner;
        inner.gateway_status = client::PortStatus::error(
            client::ChainFailure::GatewayConnectFailed, "dial failed", false,
            client::PortFailureCategory::Transport);
        LoadgenReport report;
        MetricsLoginChainTransport measured(inner, counters_for(report));
        const auto gateway = measured.connect_gateway({}, deadline);
        EXPECT_FALSE(gateway.status.ok);
        EXPECT_EQ(report.attach.attempts, 1U);
        EXPECT_EQ(report.attach.failures, 1U);
        EXPECT_EQ(
            report.attach.by_kind.at(FailureKind::ConnectionError), 1U);
        EXPECT_EQ(report.attach.latency.max(), 0.0);
        EXPECT_EQ(report.handoff.attempts, 0U);
    }
}

}  // namespace
}  // namespace realm::loadgen
