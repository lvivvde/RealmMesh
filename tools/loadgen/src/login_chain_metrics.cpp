#include "realmmesh/loadgen/login_chain_metrics.hpp"

#include <chrono>

namespace realm::loadgen {
namespace {

[[nodiscard]] double elapsed_ms(client::TimePoint started) {
    return std::chrono::duration<double, std::milli>(
               client::Clock::now() - started)
        .count();
}

[[nodiscard]] FailureKind mapped_failure(
    const client::PortStatus& status,
    FailureKind protocol_failure,
    FailureKind timeout_failure) {
    switch (status.category) {
        case client::PortFailureCategory::Transport:
            return FailureKind::ConnectionError;
        case client::PortFailureCategory::Timeout:
            return timeout_failure;
        case client::PortFailureCategory::Protocol:
            return protocol_failure;
    }
    return protocol_failure;
}

void record(PhaseCounters& counters,
            const client::PortStatus& status,
            client::TimePoint started,
            FailureKind protocol_failure,
            FailureKind timeout_failure) {
    if (status.ok) {
        counters.record_success(elapsed_ms(started));
        return;
    }
    counters.record_failure(
        mapped_failure(status, protocol_failure, timeout_failure),
        elapsed_ms(started));
}

}  // namespace

MetricsLoginChainTransport::MetricsLoginChainTransport(
    client::LoginChainTransport& inner,
    LoginChainCounters counters)
    : inner_(inner), counters_(counters) {}

client::PortValue<client::VerifyResult> MetricsLoginChainTransport::verify(
    std::string_view account,
    std::string_view credential,
    client::TimePoint deadline) {
    const auto started = client::Clock::now();
    auto result = inner_.verify(account, credential, deadline);
    record(counters_.verify, result.status, started,
           FailureKind::VerifyRejected, FailureKind::ConnectionError);
    return result;
}

client::PortValue<client::TicketResult>
MetricsLoginChainTransport::take_ticket(
    std::string_view identity_token,
    client::TimePoint deadline) {
    const auto started = client::Clock::now();
    auto result = inner_.take_ticket(identity_token, deadline);
    record(counters_.tickets, result.status, started,
           FailureKind::TicketsRejected, FailureKind::ConnectionError);
    if (result.status.ok) {
        last_number_token_ = result.value.queue_number_token;
    }
    return result;
}

client::PortValue<client::ProgressResult>
MetricsLoginChainTransport::poll_progress(client::TimePoint deadline) {
    const auto started = client::Clock::now();
    auto result = inner_.poll_progress(deadline);
    record(counters_.poll, result.status, started, FailureKind::PollFailed,
           FailureKind::ConnectionError);
    return result;
}

client::PortValue<client::TicketMeResult>
MetricsLoginChainTransport::ticket_me(
    std::string_view queue_number_token,
    client::TimePoint deadline) {
    const auto started = client::Clock::now();
    auto result = inner_.ticket_me(queue_number_token, deadline);
    record(counters_.poll, result.status, started, FailureKind::PollFailed,
           FailureKind::ConnectionError);
    return result;
}

client::PortValue<std::unique_ptr<client::GatewaySession>>
MetricsLoginChainTransport::connect_gateway(
    std::span<const network::client::EndpointCandidate> candidates,
    client::TimePoint deadline) {
    auto result = inner_.connect_gateway(candidates, deadline);
    // 旧口径：拨号成功不进入 attach 时延；拨号失败仍算一次 attach
    // connection_error，样本从 attach 起点计为 0。
    if (!result.status.ok || result.value == nullptr) {
        counters_.attach.record_failure(FailureKind::ConnectionError, 0.0);
    }
    return result;
}

client::PortStatus MetricsLoginChainTransport::attach(
    client::GatewaySession& session,
    std::string_view identity_token,
    std::string_view admission_grant,
    client::TimePoint deadline) {
    const auto started = client::Clock::now();
    auto status = inner_.attach(
        session, identity_token, admission_grant, deadline);
    record(counters_.attach, status, started, FailureKind::AttachRejected,
           FailureKind::AttachTimeout);
    return status;
}

client::PortValue<client::HandoffResult>
MetricsLoginChainTransport::await_handoff(
    client::GatewaySession& session,
    client::TimePoint deadline) {
    const auto started = client::Clock::now();
    auto result = inner_.await_handoff(session, deadline);
    record(counters_.handoff, result.status, started,
           FailureKind::HandoffRejected, FailureKind::HandoffTimeout);
    return result;
}

client::PortValue<std::unique_ptr<client::RealmSession>>
MetricsLoginChainTransport::connect_realm(
    std::span<const network::client::EndpointCandidate> candidates,
    client::TimePoint deadline) {
    return inner_.connect_realm(candidates, deadline);
}

client::PortStatus MetricsLoginChainTransport::enter_realm(
    client::RealmSession& session,
    std::string_view enter_realm_ticket,
    client::TimePoint deadline) {
    return inner_.enter_realm(session, enter_realm_ticket, deadline);
}

}  // namespace realm::loadgen
