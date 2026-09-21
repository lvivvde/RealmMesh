#include "realmmesh/game/gateway/gateway_login_pipeline.hpp"

#include "realmmesh/game/common/compact_jws.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/queue_number.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/gateway_admission.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/observability/metrics_registry.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realm::game::gateway {
namespace {

constexpr std::size_t max_events_per_advance = 4'096;
constexpr std::chrono::seconds enter_realm_ticket_ttl{60};

enum class PublicStage : std::uint8_t {
    Pending,
    Fetching,
    HandedOff,
};

enum class IntentKind : std::uint8_t {
    Decline,
    Close,
    Accept,
    Handoff,
};

struct RuntimeIntent {
    IntentKind kind{IntentKind::Close};
    std::vector<std::byte> payload;
};

struct PipelineSession {
    PublicStage stage{PublicStage::Pending};
    std::optional<RuntimeIntent> intent;
    bool closing{false};
    bool fetch_reserved{false};
    std::uint64_t account_id{0};
    std::string reserved_jti;
    std::chrono::system_clock::time_point jti_expires_at{};
    std::optional<GatewayAdmissionReservation> admission_reservation;
    unsigned fetch_failures{0};
    std::chrono::steady_clock::time_point fetch_due{};
    std::optional<AccountFetchAttemptId> active_attempt;
    std::optional<std::chrono::steady_clock::time_point> handoff_deadline;
    std::string source;
    std::uint32_t attach_attempts{0};
};

[[nodiscard]] std::vector<std::byte> error_payload(
    int code,
    std::string_view message,
    std::uint64_t request_id,
    std::chrono::seconds retry_after = std::chrono::seconds::zero()) {
    common::EdgeError error;
    error.set_code(static_cast<std::uint32_t>(code));
    error.set_message(std::string(message));
    if (retry_after > std::chrono::seconds::zero()) {
        error.set_retry_after_seconds(static_cast<std::uint32_t>(
            std::min<std::int64_t>(
                retry_after.count(),
                std::numeric_limits<std::uint32_t>::max())));
    }
    return common::encode(error, request_id);
}

[[nodiscard]] std::size_t completion_limit(std::uint64_t capacity) {
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        capacity,
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())));
}

[[nodiscard]] std::chrono::milliseconds retry_delay(
    std::chrono::milliseconds base, unsigned failure_count) {
    const auto shift = std::min(failure_count - 1U, 30U);
    const auto multiplier = std::uint64_t{1} << shift;
    const auto maximum =
        static_cast<std::uint64_t>(std::chrono::milliseconds::max().count());
    const auto count = static_cast<std::uint64_t>(base.count());
    if (count > maximum / multiplier) {
        return std::chrono::milliseconds::max();
    }
    return std::chrono::milliseconds{count * multiplier};
}

}  // namespace

class GatewayLoginPipeline::Impl final {
public:
    Impl(
        GatewayLoginConfig config,
        GatewaySigningMaterial signing_material,
        GatewayPrimaryTransport& primary_transport,
        AccountFetchPort& account_fetch,
        observability::Logger* logger,
        observability::MetricsRegistry* metrics)
        : config_(std::move(config)),
          ingress_(config_.credential_ingress),
          identity_codec_(
              std::in_place,
              signing_material.identity_seed,
              signing_material.identity_kid),
          number_codec_(
              std::in_place,
              signing_material.queue_seed,
              signing_material.queue_kid),
          tickets_(signing_material.enter_realm_key),
          identity_issuer_(std::move(signing_material.identity_issuer)),
          primary_transport_(&primary_transport),
          account_fetch_(&account_fetch),
          logger_(logger),
          metrics_(metrics) {}

    Impl(
        GatewayLoginConfig config,
        common::SessionTicketKey enter_realm_key,
        GatewayAdmission& admission,
        std::string gateway_instance,
        GatewayPrimaryTransport& primary_transport,
        AccountFetchPort& account_fetch,
        observability::Logger* logger,
        observability::MetricsRegistry* metrics)
        : config_(std::move(config)),
          ingress_(config_.credential_ingress),
          tickets_(enter_realm_key),
          admission_(&admission),
          gateway_instance_(std::move(gateway_instance)),
          primary_transport_(&primary_transport),
          account_fetch_(&account_fetch),
          logger_(logger),
          metrics_(metrics) {}

    ~Impl() {
        for (auto& [session_id, session] : sessions_) {
            static_cast<void>(session_id);
            if (session.active_attempt.has_value()) {
                account_fetch_->cancel(*session.active_attempt);
            }
            release_jti_reservation(session);
        }
    }

    [[nodiscard]] GatewayLoginAdvanceResult advance(GatewayLoginFrame frame) {
        if (health_ == GatewayPipelineHealth::Unhealthy) {
            publish_metrics();
            return result();
        }

        verification_now_ = frame.verification_now;
        if (admission_ != nullptr) {
            static_cast<void>(admission_->refresh_availability(frame.now));
        }
        evict_jtis(frame.verification_now);
        auto events = primary_transport_->drain_events(max_events_per_advance);
        preapply_lifecycle(events);
        process_messages(events, frame);
        apply_handoff_expiry(frame.now);
        drain_fetch_completions(frame);
        drive_fetch_submissions(frame.now);
        flush_intents(frame);
        publish_metrics();
        return result();
    }

private:
    void preapply_lifecycle(const std::vector<GatewayEvent>& events) {
        for (const auto& event : events) {
            if (event.kind == GatewayEventKind::SessionOpened) {
                if (!sessions_.contains(event.session_id) &&
                    sessions_.size() < config_.conn_capacity) {
                    PipelineSession session;
                    session.source = event.source;
                    sessions_.emplace(event.session_id, std::move(session));
                }
                continue;
            }
            if (event.kind == GatewayEventKind::PeerAddressChanged) {
                const auto found = sessions_.find(event.session_id);
                if (found != sessions_.end()) {
                    found->second.source = event.source;
                }
                continue;
            }
            if (event.kind != GatewayEventKind::SessionClosed) continue;
            const auto found = sessions_.find(event.session_id);
            if (found == sessions_.end()) continue;
            release_session(found->second);
            sessions_.erase(found);
        }
    }

    void process_messages(
        const std::vector<GatewayEvent>& events,
        const GatewayLoginFrame& frame) {
        for (const auto& event : events) {
            if (event.kind != GatewayEventKind::MessageReceived) continue;
            const auto found = sessions_.find(event.session_id);
            if (found == sessions_.end() || found->second.closing ||
                found->second.intent.has_value()) {
                continue;
            }
            auto& session = found->second;
            const auto request_id =
                common::edge_request_id(event.payload).value_or(0);
            if (event.established || session.stage != PublicStage::Pending) {
                schedule_close(session);
                continue;
            }
            // Keep the pre-ingress envelope guard cheap and bounded. Non-attach
            // messages (including retired IDs) retain the existing
            // not-authenticated wire response; only a recognized attach enters
            // credential normalization/rate limiting.
            if (event.payload.size() >
                config_.credential_ingress.max_attach_envelope_bytes) {
                schedule_close(session);
                continue;
            }
            const auto message_id = common::edge_message_id(event.payload);
            if (!message_id.has_value() ||
                *message_id !=
                    common::EdgeMessageId::MESSAGE_ID_C2S_EDGE_ATTACH) {
                schedule_decline(
                    session,
                    common::edge_error_not_authenticated,
                    "attach before any other message",
                    request_id);
                continue;
            }
            ++session.attach_attempts;
            auto checked = ingress_.inspect_attach(
                session.source,
                session.attach_attempts,
                event.payload,
                frame.now);
            switch (checked.status) {
            case GatewayIngressStatus::Allowed:
                handle_attach(event, *checked.attach, frame);
                break;
            case GatewayIngressStatus::SourceThrottled:
            case GatewayIngressStatus::VerificationSaturated:
                schedule_decline(
                    session,
                    common::edge_error_throttled,
                    "admission throttled",
                    request_id,
                    checked.retry_after);
                break;
            case GatewayIngressStatus::Oversized:
            case GatewayIngressStatus::SustainedAbuse:
            case GatewayIngressStatus::SessionLimited:
                schedule_close(session);
                break;
            case GatewayIngressStatus::Malformed:
                schedule_decline(
                    session,
                    common::edge_error_invalid_credentials,
                    "invalid credentials",
                    request_id);
                break;
            }
        }
    }

    void handle_attach(
        const GatewayEvent& event,
        const common::EdgeAttach& attach,
        const GatewayLoginFrame& frame) {
        auto& session = sessions_.at(event.session_id);
        const auto request_id =
            common::edge_request_id(event.payload).value_or(0);
        if (session.stage != PublicStage::Pending) {
            schedule_decline(
                session,
                common::edge_error_invalid_credentials,
                "invalid credentials",
                request_id);
            return;
        }
        if (fetch_used_ >= config_.fetch_capacity) {
            schedule_decline(
                session,
                common::edge_error_attach_out_of_budget,
                "attach out of budget",
                request_id);
            return;
        }

        if (admission_ != nullptr) {
            const auto owner = gateway_instance_ + "/" +
                std::to_string(event.session_id.value) + "/" +
                std::to_string(next_admission_attempt_id_++);
            // Admission Grant is the only credential accepted at this seam;
            // Queue Number remains a queue-position credential only.
            auto started = admission_->reserve(
                attach.identity_token(),
                attach.admission_grant(),
                owner,
                frame.verification_now);
            if (started.status != GatewayAdmissionStartStatus::Reserved ||
                !started.reservation.has_value()) {
                if (started.status == GatewayAdmissionStartStatus::Consumed) {
                    ++replay_rejections_;
                }
                switch (started.status) {
                case GatewayAdmissionStartStatus::InProgress:
                    record_credential_result("in_progress");
                    schedule_decline(
                        session,
                        common::edge_error_admission_in_progress,
                        "admission in progress",
                        request_id);
                    break;
                case GatewayAdmissionStartStatus::StoreUnavailable:
                    record_credential_result("store_unavailable");
                    schedule_decline(
                        session,
                        common::edge_error_admission_unavailable,
                        "admission unavailable",
                        request_id);
                    break;
                case GatewayAdmissionStartStatus::Reserved:
                case GatewayAdmissionStartStatus::InvalidCredentials:
                case GatewayAdmissionStartStatus::Consumed:
                    record_credential_result("invalid");
                    schedule_decline(
                        session,
                        common::edge_error_invalid_credentials,
                        "invalid credentials",
                        request_id);
                    break;
                }
                return;
            }
            record_credential_result("reserved");

            session.account_id = started.reservation->account_id();
            session.admission_reservation = std::move(*started.reservation);
            session.fetch_reserved = true;
            ++fetch_used_;

            common::EdgeAttachAccepted accepted;
            accepted.set_account_id(session.account_id);
            session.intent = RuntimeIntent{
                IntentKind::Accept, common::encode(accepted, request_id)};
            return;
        }

        const auto identity = identity_codec_->validate(
            attach.identity_token(), identity_issuer_, frame.verification_now);
        if (!identity.has_value()) {
            record_credential_result("invalid");
            schedule_decline(
                session,
                common::edge_error_invalid_credentials,
                "invalid credentials",
                request_id);
            return;
        }
        const auto number = number_codec_->validate(
            attach.admission_grant(), frame.verification_now);
        if (!number.has_value() || !number->admitted) {
            record_credential_result("invalid");
            schedule_decline(
                session,
                common::edge_error_invalid_queue_number,
                "invalid queue number",
                request_id);
            return;
        }
        if (reserved_jtis_.contains(identity->jti) ||
            consumed_jtis_.contains(identity->jti)) {
            ++replay_rejections_;
            record_credential_result("invalid");
            schedule_decline(
                session,
                common::edge_error_invalid_credentials,
                "invalid credentials",
                request_id);
            return;
        }

        reserved_jtis_.emplace(identity->jti, identity->expires_at);
        session.reserved_jti = identity->jti;
        session.jti_expires_at = identity->expires_at;
        session.account_id = identity->account_id;
        session.fetch_reserved = true;
        ++fetch_used_;

        common::EdgeAttachAccepted accepted;
        accepted.set_account_id(identity->account_id);
        session.intent = RuntimeIntent{
            IntentKind::Accept, common::encode(accepted, request_id)};
        record_credential_result("reserved");
    }

    void apply_handoff_expiry(std::chrono::steady_clock::time_point now) {
        for (auto& [session_id, session] : sessions_) {
            static_cast<void>(session_id);
            if (session.closing || !session.handoff_deadline.has_value() ||
                *session.handoff_deadline > now) {
                continue;
            }
            if (logger_ != nullptr) {
                static_cast<void>(logger_->warn(
                    "edge_handoff_expired",
                    "handoff grace elapsed without migration; closing session",
                    {observability::field(
                        "session_id",
                        session_id.value,
                        observability::DataClass::Internal)}));
            }
            schedule_close(session);
        }
    }

    void drain_fetch_completions(const GatewayLoginFrame& frame) {
        for (const auto& completion : account_fetch_->drain_completions(
                 frame.now, completion_limit(config_.fetch_capacity))) {
            const auto attempt = attempts_.find(completion.attempt_id.value);
            if (attempt == attempts_.end()) continue;
            const auto found = sessions_.find(attempt->second);
            if (found == sessions_.end() ||
                !found->second.active_attempt.has_value() ||
                *found->second.active_attempt != completion.attempt_id) {
                attempts_.erase(attempt);
                continue;
            }
            auto& session = found->second;
            const auto session_id = found->first;
            session.active_attempt.reset();
            attempts_.erase(attempt);
            if (session.closing) continue;
            if (completion.ok) {
                release_fetch_reservation(session);
                session.intent = RuntimeIntent{IntentKind::Handoff, {}};
                if (metrics_ != nullptr) {
                    metrics_->histogram_observe(
                        "edge_fetch_duration_seconds",
                        std::chrono::duration<double>(completion.duration)
                            .count());
                }
                if (logger_ != nullptr) {
                    static_cast<void>(logger_->info(
                        "edge_fetch_completed",
                        "edge fetch completed",
                        {observability::field(
                            "session_id",
                            session_id.value,
                            observability::DataClass::Internal)}));
                }
                continue;
            }

            ++session.fetch_failures;
            if (session.fetch_failures > config_.fetch_retry_max) {
                release_fetch_reservation(session);
                if (logger_ != nullptr) {
                    static_cast<void>(logger_->warn(
                        "edge_fetch_exhausted",
                        "edge fetch exhausted; closing session",
                        {observability::field(
                            "session_id",
                            session_id.value,
                            observability::DataClass::Internal)}));
                }
                schedule_close(session);
            } else {
                session.fetch_due = frame.now + retry_delay(
                                                    config_.fetch_retry_base,
                                                    session.fetch_failures);
            }
        }
    }

    void drive_fetch_submissions(std::chrono::steady_clock::time_point now) {
        for (auto& [session_id, session] : sessions_) {
            if (health_ == GatewayPipelineHealth::Unhealthy) return;
            if (session.closing || session.stage != PublicStage::Fetching ||
                !session.fetch_reserved || session.active_attempt.has_value() ||
                session.intent.has_value() || session.fetch_due > now) {
                continue;
            }
            const AccountFetchAttemptId attempt_id{next_attempt_id_++};
            const auto submit = account_fetch_->submit(
                {attempt_id, session_id, session.account_id}, now);
            if (submit == AccountFetchSubmitResult::Full) {
                ++fetch_submit_backpressure_;
                continue;
            }
            if (submit == AccountFetchSubmitResult::Stopped) {
                mark_unhealthy();
                return;
            }
            session.active_attempt = attempt_id;
            attempts_.emplace(attempt_id.value, session_id);
            if (session.fetch_failures > 0) ++fetch_retry_total_;
        }
    }

    void flush_intents(const GatewayLoginFrame& frame) {
        static constexpr std::array priority{
            IntentKind::Close,
            IntentKind::Decline,
            IntentKind::Accept,
            IntentKind::Handoff,
        };
        std::vector<EdgeSessionId> ordered;
        ordered.reserve(sessions_.size());
        for (const auto& [session_id, session] : sessions_) {
            static_cast<void>(session);
            ordered.push_back(session_id);
        }
        std::ranges::sort(ordered, {}, &EdgeSessionId::value);

        for (const auto kind : priority) {
            for (const auto session_id : ordered) {
                if (health_ == GatewayPipelineHealth::Unhealthy) return;
                const auto found = sessions_.find(session_id);
                if (found == sessions_.end() ||
                    !found->second.intent.has_value() ||
                    found->second.intent->kind != kind) {
                    continue;
                }
                flush_intent(session_id, found->second, frame);
            }
        }
    }

    void flush_intent(
        EdgeSessionId session_id,
        PipelineSession& session,
        const GatewayLoginFrame& frame) {
        auto& intent = *session.intent;
        const auto intent_kind = intent.kind;
        PrimaryTransportResult result = PrimaryTransportResult::Stopped;
        switch (intent_kind) {
        case IntentKind::Close:
            result = primary_transport_->close(session_id);
            break;
        case IntentKind::Decline:
            result = primary_transport_->decline(session_id, intent.payload);
            break;
        case IntentKind::Accept:
            result = primary_transport_->accept(session_id, intent.payload);
            break;
        case IntentKind::Handoff: {
            const auto endpoint = frame.discovered_realm.has_value()
                                      ? frame.discovered_realm
                                      : config_.static_realm;
            if (!endpoint.has_value()) {
                if (logger_ != nullptr) {
                    static_cast<void>(logger_->warn(
                        "edge_handoff_unavailable",
                        "no realm endpoint for handoff; closing session",
                        {observability::field(
                            "session_id",
                            session_id.value,
                            observability::DataClass::Internal)}));
                }
                schedule_close(session);
                return;
            }
            result = primary_transport_->send_handoff(
                session_id,
                handoff_payload(
                    session.account_id, *endpoint, frame.verification_now));
            if (result == PrimaryTransportResult::Queued) {
                session.stage = PublicStage::HandedOff;
                session.handoff_deadline = frame.now + config_.handoff_grace;
                if (logger_ != nullptr) {
                    static_cast<void>(logger_->info(
                        "edge_handoff_granted",
                        "enter-realm handoff granted",
                        {observability::field(
                             "session_id",
                             session_id.value,
                             observability::DataClass::Internal),
                         observability::field(
                             "account_id",
                             session.account_id,
                             observability::DataClass::Pseudonymous)}));
                }
            }
            break;
        }
        }
        std::optional<GatewayAdmissionTransitionStatus> admission_transition;
        if (intent_kind == IntentKind::Accept && admission_ != nullptr) {
            if (!session.admission_reservation.has_value()) {
                mark_unhealthy();
                return;
            }
            admission_transition = admission_->on_accept_result(
                *session.admission_reservation,
                result,
                frame.verification_now);
        }
        if (result == PrimaryTransportResult::Full) {
            ++runtime_backpressure_[static_cast<std::size_t>(intent_kind)];
            return;
        }
        if (result == PrimaryTransportResult::Stopped) {
            session.admission_reservation.reset();
            mark_unhealthy();
            return;
        }

        if (intent_kind == IntentKind::Accept) {
            if (admission_transition.has_value()) {
                session.admission_reservation.reset();
                if (*admission_transition !=
                    GatewayAdmissionTransitionStatus::Applied) {
                    // accept 已进入 Primary Transport，存储提交若丢失或
                    // 结果不确定，绝不继续 Fetching，也绝不补 release。
                    release_fetch_reservation(session);
                    session.closing = true;
                    session.intent = RuntimeIntent{IntentKind::Close, {}};
                    return;
                }
            }
            commit_accept(session_id, session, frame.now);
        } else if (
            intent_kind == IntentKind::Close ||
            intent_kind == IntentKind::Decline) {
            session.closing = true;
            if (intent_kind == IntentKind::Close) {
                session.handoff_deadline.reset();
            }
        }
        session.intent.reset();
    }

    void commit_accept(
        EdgeSessionId session_id,
        PipelineSession& session,
        std::chrono::steady_clock::time_point now) {
        if (admission_ == nullptr) {
            const auto reserved = reserved_jtis_.find(session.reserved_jti);
            if (reserved != reserved_jtis_.end()) {
                reserved_jtis_.erase(reserved);
            }
            consumed_jtis_.insert_or_assign(
                session.reserved_jti, session.jti_expires_at);
            session.reserved_jti.clear();
        }
        session.stage = PublicStage::Fetching;
        session.fetch_due = now;
        if (logger_ != nullptr) {
            static_cast<void>(logger_->info(
                "edge_session_attached",
                "edge session attached",
                {observability::field(
                     "session_id",
                     session_id.value,
                     observability::DataClass::Internal),
                 observability::field(
                     "account_id",
                     session.account_id,
                     observability::DataClass::Pseudonymous)}));
        }
    }

    [[nodiscard]] std::vector<std::byte> handoff_payload(
        std::uint64_t account_id,
        const RealmEndpoint& endpoint,
        std::chrono::system_clock::time_point now) const {
        const auto ticket = tickets_.issue(
            common::TicketPurpose::EnterRealm,
            account_id,
            1,
            0,
            enter_realm_ticket_ttl,
            now);
        common::EnterRealmGranted granted;
        granted.set_enter_realm_ticket(ticket.data(), ticket.size());
        auto* wire_endpoint = granted.add_realm_endpoints();
        wire_endpoint->set_protocol(
            ::realmmesh::protocol::edge::v1::TRANSPORT_PROTOCOL_TLS_TCP);
        wire_endpoint->set_address(endpoint.address);
        wire_endpoint->set_port(endpoint.port);
        wire_endpoint->set_priority(0);
        return common::encode(granted, 0);
    }

    void schedule_decline(
        PipelineSession& session,
        int code,
        std::string_view message,
        std::uint64_t request_id,
        std::chrono::seconds retry_after = std::chrono::seconds::zero()) {
        if (session.active_attempt.has_value()) {
            account_fetch_->cancel(*session.active_attempt);
            attempts_.erase(session.active_attempt->value);
            session.active_attempt.reset();
        }
        release_fetch_reservation(session);
        release_jti_reservation(session);
        session.closing = true;
        session.intent = RuntimeIntent{
            IntentKind::Decline,
            error_payload(code, message, request_id, retry_after)};
    }

    void record_credential_result(std::string_view result) {
        auto index = credential_results_.size() - 1U;
        if (result == "reserved") index = 0;
        else if (result == "invalid") index = 1;
        else if (result == "in_progress") index = 2;
        else if (result == "store_unavailable") index = 3;
        ++credential_results_[index];
        if (logger_ != nullptr && admission_ != nullptr) {
            static_cast<void>(logger_->info(
                "gateway_credential_audit",
                "gateway credential admission outcome",
                {observability::field("gateway_instance", gateway_instance_),
                 observability::field("result", result)}));
        }
    }

    void schedule_close(PipelineSession& session) {
        if (session.active_attempt.has_value()) {
            account_fetch_->cancel(*session.active_attempt);
            attempts_.erase(session.active_attempt->value);
            session.active_attempt.reset();
        }
        release_fetch_reservation(session);
        release_jti_reservation(session);
        session.closing = true;
        session.intent = RuntimeIntent{IntentKind::Close, {}};
    }

    void release_session(PipelineSession& session) {
        if (session.active_attempt.has_value()) {
            account_fetch_->cancel(*session.active_attempt);
            attempts_.erase(session.active_attempt->value);
        }
        release_fetch_reservation(session);
        release_jti_reservation(session);
    }

    void release_fetch_reservation(PipelineSession& session) {
        if (!session.fetch_reserved) return;
        session.fetch_reserved = false;
        --fetch_used_;
    }

    void release_jti_reservation(PipelineSession& session) {
        if (session.admission_reservation.has_value()) {
            static_cast<void>(admission_->abandon(
                *session.admission_reservation, verification_now_));
            session.admission_reservation.reset();
            return;
        }
        if (session.reserved_jti.empty()) return;
        reserved_jtis_.erase(session.reserved_jti);
        session.reserved_jti.clear();
    }

    void evict_jtis(std::chrono::system_clock::time_point now) {
        const auto expired = [now](const auto& entry) {
            return entry.second + common::jws_clock_leeway < now;
        };
        std::erase_if(reserved_jtis_, expired);
        std::erase_if(consumed_jtis_, expired);
    }

    void mark_unhealthy() {
        health_ = GatewayPipelineHealth::Unhealthy;
        for (auto& [session_id, session] : sessions_) {
            static_cast<void>(session_id);
            if (session.active_attempt.has_value()) {
                account_fetch_->cancel(*session.active_attempt);
                session.active_attempt.reset();
            }
            release_jti_reservation(session);
        }
        attempts_.clear();
    }

    void publish_metrics() {
        if (metrics_ == nullptr) return;
        std::array<std::uint64_t, 3> counts{};
        for (const auto& [session_id, session] : sessions_) {
            static_cast<void>(session_id);
            ++counts[static_cast<std::size_t>(session.stage)];
        }
        metrics_->gauge_set(
            "edge_sessions",
            static_cast<double>(counts[0]),
            {{"stage", "pending"}});
        metrics_->gauge_set(
            "edge_sessions",
            static_cast<double>(counts[1]),
            {{"stage", "fetching"}});
        metrics_->gauge_set(
            "edge_sessions",
            static_cast<double>(counts[2]),
            {{"stage", "handed_off"}});
        metrics_->gauge_set(
            "edge_budget",
            static_cast<double>(conn_free()),
            {{"kind", "conn_free"}});
        metrics_->gauge_set(
            "edge_budget",
            static_cast<double>(fetch_free()),
            {{"kind", "fetch_free"}});
        metrics_->counter_set(
            "edge_fetch_retry_total", static_cast<double>(fetch_retry_total_));
        metrics_->counter_set(
            "edge_jti_replay_rejected_total",
            static_cast<double>(replay_rejections_));
        metrics_->counter_set(
            "edge_fetch_submit_backpressure_total",
            static_cast<double>(fetch_submit_backpressure_));
        const auto ingress = ingress_.counters();
        static constexpr std::array ingress_labels{
            "oversized",
            "malformed",
            "source_throttled",
            "sustained_abuse",
            "session_limited",
            "verification_saturated",
        };
        const std::array ingress_values{
            ingress.oversized,
            ingress.malformed,
            ingress.source_throttled,
            ingress.sustained_abuse,
            ingress.session_limited,
            ingress.verification_saturated,
        };
        for (std::size_t index = 0; index < ingress_labels.size(); ++index) {
            metrics_->counter_set(
                "edge_credential_ingress_rejected_total",
                static_cast<double>(ingress_values[index]),
                {{"reason", ingress_labels[index]}});
        }
        metrics_->gauge_set(
            "edge_credential_verification_in_flight",
            static_cast<double>(ingress.verification_in_flight));
        static constexpr std::array result_labels{
            "reserved", "invalid", "in_progress", "store_unavailable", "other"};
        for (std::size_t index = 0; index < result_labels.size(); ++index) {
            metrics_->counter_set(
                "edge_credential_result_total",
                static_cast<double>(credential_results_[index]),
                {{"result", result_labels[index]}});
        }
        static constexpr std::array labels{
            "decline", "close", "accept", "handoff"};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            metrics_->counter_set(
                "edge_runtime_command_backpressure_total",
                static_cast<double>(runtime_backpressure_[index]),
                {{"command", labels[index]}});
        }
    }

    [[nodiscard]] std::uint64_t conn_free() const {
        return config_.conn_capacity - sessions_.size();
    }

    [[nodiscard]] std::uint64_t fetch_free() const {
        return config_.fetch_capacity - fetch_used_;
    }

    [[nodiscard]] GatewayLoginAdvanceResult result() const {
        return {
            .health = health_,
            .local_budget =
                {
                    .conn_free = conn_free(),
                    .fetch_free = fetch_free(),
                    .available =
                        health_ == GatewayPipelineHealth::Healthy &&
                        (admission_ == nullptr || admission_->available()),
                },
        };
    }

    GatewayLoginConfig config_;
    GatewayCredentialIngress ingress_;
    std::optional<common::IdentityTokenCodec> identity_codec_;
    std::optional<common::QueueNumberCodec> number_codec_;
    common::SessionTickets tickets_;
    std::string identity_issuer_;
    GatewayAdmission* admission_{nullptr};
    std::string gateway_instance_;
    GatewayPrimaryTransport* primary_transport_;
    AccountFetchPort* account_fetch_;
    observability::Logger* logger_;
    observability::MetricsRegistry* metrics_;
    GatewayPipelineHealth health_{GatewayPipelineHealth::Healthy};
    std::unordered_map<EdgeSessionId, PipelineSession> sessions_;
    std::unordered_map<std::string, std::chrono::system_clock::time_point>
        reserved_jtis_;
    std::unordered_map<std::string, std::chrono::system_clock::time_point>
        consumed_jtis_;
    std::unordered_map<std::uint64_t, EdgeSessionId> attempts_;
    std::uint64_t fetch_used_{0};
    std::uint64_t next_attempt_id_{1};
    std::uint64_t next_admission_attempt_id_{1};
    std::uint64_t fetch_retry_total_{0};
    std::uint64_t replay_rejections_{0};
    std::uint64_t fetch_submit_backpressure_{0};
    std::array<std::uint64_t, 4> runtime_backpressure_{};
    std::array<std::uint64_t, 5> credential_results_{};
    std::chrono::system_clock::time_point verification_now_{};
};

GatewayLoginPipeline GatewayLoginPipeline::create(
    GatewayLoginConfig config,
    GatewaySigningMaterial signing_material,
    GatewayPrimaryTransport& primary_transport,
    AccountFetchPort& account_fetch,
    observability::Logger* logger,
    observability::MetricsRegistry* metrics) {
    config.validate();
    if (signing_material.identity_kid.empty() ||
        signing_material.queue_kid.empty() ||
        signing_material.identity_issuer.empty()) {
        throw std::invalid_argument(
            "gateway signing identifiers must not be empty");
    }
    return GatewayLoginPipeline(
        std::make_unique<Impl>(
            std::move(config),
            std::move(signing_material),
            primary_transport,
            account_fetch,
            logger,
            metrics));
}

GatewayLoginPipeline GatewayLoginPipeline::create(
    GatewayLoginConfig config,
    common::SessionTicketKey enter_realm_key,
    GatewayAdmission& admission,
    std::string gateway_instance,
    GatewayPrimaryTransport& primary_transport,
    AccountFetchPort& account_fetch,
    observability::Logger* logger,
    observability::MetricsRegistry* metrics) {
    config.validate();
    if (gateway_instance.empty() || gateway_instance.size() > 64U ||
        !std::ranges::all_of(gateway_instance, [](char character) {
            return character >= '!' && character <= '~';
        })) {
        throw std::invalid_argument("invalid gateway instance identifier");
    }
    return GatewayLoginPipeline(std::make_unique<Impl>(
        std::move(config),
        enter_realm_key,
        admission,
        std::move(gateway_instance),
        primary_transport,
        account_fetch,
        logger,
        metrics));
}

GatewayLoginPipeline::GatewayLoginPipeline(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GatewayLoginPipeline::~GatewayLoginPipeline() = default;
GatewayLoginPipeline::GatewayLoginPipeline(GatewayLoginPipeline&&) noexcept =
    default;
GatewayLoginPipeline& GatewayLoginPipeline::operator=(
    GatewayLoginPipeline&&) noexcept = default;

GatewayLoginAdvanceResult GatewayLoginPipeline::advance(
    GatewayLoginFrame frame) {
    return impl_->advance(std::move(frame));
}

}  // namespace realm::game::gateway
