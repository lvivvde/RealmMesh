#include "realmmesh/client/login_stage.hpp"

namespace realm::client {

std::string_view login_stage_name(LoginStage stage) noexcept {
    switch (stage) {
        case LoginStage::Idle:
            return "idle";
        case LoginStage::Verifying:
            return "verifying";
        case LoginStage::Queued:
            return "queued";
        case LoginStage::Admitted:
            return "admitted";
        case LoginStage::GatewayConnecting:
            return "gateway_connecting";
        case LoginStage::HandoffReceived:
            return "handoff_received";
        case LoginStage::RealmConnecting:
            return "realm_connecting";
        case LoginStage::InGame:
            return "in_game";
    }
    return "unknown";
}

std::string_view chain_failure_name(ChainFailure failure) noexcept {
    switch (failure) {
        case ChainFailure::None:
            return "none";
        case ChainFailure::VerifyRejected:
            return "verify_rejected";
        case ChainFailure::TicketRejected:
            return "ticket_rejected";
        case ChainFailure::ProgressFailed:
            return "progress_failed";
        case ChainFailure::AdmitTimeout:
            return "admit_timeout";
        case ChainFailure::GatewayConnectFailed:
            return "gateway_connect_failed";
        case ChainFailure::AttachRejected:
            return "attach_rejected";
        case ChainFailure::HandoffTimeout:
            return "handoff_timeout";
        case ChainFailure::HandoffRejected:
            return "handoff_rejected";
        case ChainFailure::RealmConnectFailed:
            return "realm_connect_failed";
        case ChainFailure::EnterRealmRejected:
            return "enter_realm_rejected";
        case ChainFailure::DeadlineExceeded:
            return "deadline_exceeded";
    }
    return "unknown";
}

}  // namespace realm::client
