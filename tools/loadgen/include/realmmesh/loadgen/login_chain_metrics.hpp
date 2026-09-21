#pragma once

#include "realmmesh/client/login_chain.hpp"
#include "realmmesh/loadgen/stats.hpp"

namespace realm::loadgen {

struct LoginChainCounters final {
    PhaseCounters& verify;
    PhaseCounters& tickets;
    PhaseCounters& poll;
    PhaseCounters& attach;
    PhaseCounters& handoff;
};

/// LoginChainTransport Decorator：只把既有端口动词翻译到既有五相位
/// 计数器，不观察 LoginChain 的内部阶段。
class MetricsLoginChainTransport final : public client::LoginChainTransport {
public:
    MetricsLoginChainTransport(client::LoginChainTransport& inner,
                               LoginChainCounters counters);

    /// 保留旧报告的号码牌采集能力；仅暴露最近一次成功取号的原始值。
    [[nodiscard]] const std::string& last_number_token() const noexcept {
        return last_number_token_;
    }

    client::PortValue<client::VerifyResult> verify(
        std::string_view account,
        std::string_view credential,
        client::TimePoint deadline) override;
    client::PortValue<client::TicketResult> take_ticket(
        std::string_view identity_token,
        client::TimePoint deadline) override;
    client::PortValue<client::ProgressResult> poll_progress(
        client::TimePoint deadline) override;
    client::PortValue<client::TicketMeResult> ticket_me(
        std::string_view queue_number_token,
        client::TimePoint deadline) override;

    client::PortValue<std::unique_ptr<client::GatewaySession>> connect_gateway(
        std::span<const network::client::EndpointCandidate> candidates,
        client::TimePoint deadline) override;
    client::PortStatus attach(
        client::GatewaySession& session,
        std::string_view identity_token,
        std::string_view admission_grant,
        client::TimePoint deadline) override;
    client::PortValue<client::HandoffResult> await_handoff(
        client::GatewaySession& session,
        client::TimePoint deadline) override;

    client::PortValue<std::unique_ptr<client::RealmSession>> connect_realm(
        std::span<const network::client::EndpointCandidate> candidates,
        client::TimePoint deadline) override;
    client::PortStatus enter_realm(
        client::RealmSession& session,
        std::string_view enter_realm_ticket,
        client::TimePoint deadline) override;

private:
    client::LoginChainTransport& inner_;
    LoginChainCounters counters_;
    std::string last_number_token_;
};

}  // namespace realm::loadgen
