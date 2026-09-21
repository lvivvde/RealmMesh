#include "realmmesh/client/login_chain.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace realm::client {
namespace {

[[nodiscard]] std::chrono::milliseconds time_left(TimePoint deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    return left.count() <= 0 ? std::chrono::milliseconds{0} : left;
}

}  // namespace

PortStatus PortStatus::success() {
    PortStatus status;
    status.ok = true;
    return status;
}

PortStatus PortStatus::error(ChainFailure failure,
                             std::string detail,
                             bool credential_expired,
                             PortFailureCategory category,
                             PortRecovery recovery,
                             std::chrono::seconds retry_after) {
    PortStatus status;
    status.failure = failure;
    status.credential_expired = credential_expired;
    status.category = category;
    status.recovery = recovery;
    status.retry_after = retry_after;
    status.detail = std::move(detail);
    return status;
}

LoginRun::LoginRun(LoginTarget target,
                   std::string account,
                   std::string credential,
                   TimePoint deadline,
                   PollingProfile profile,
                   TimePoint hold_until)
    : target_(target), account_(std::move(account)),
      credential_(std::move(credential)), deadline_(deadline),
      polling_profile_(profile), hold_until_(hold_until) {}

LoginRun LoginRun::verify(std::string account,
                          std::string credential,
                          TimePoint deadline) {
    return LoginRun{LoginTarget::Verify, std::move(account),
                    std::move(credential), deadline,
                    PollingProfile::ClientRealistic};
}

LoginRun LoginRun::tickets(std::string account,
                           std::string credential,
                           TimePoint deadline) {
    return LoginRun{LoginTarget::Tickets, std::move(account),
                    std::move(credential), deadline,
                    PollingProfile::ClientRealistic};
}

LoginRun LoginRun::poll(std::string account,
                        std::string credential,
                        TimePoint deadline,
                        PollingProfile profile) {
    return LoginRun{LoginTarget::Poll, std::move(account),
                    std::move(credential), deadline, profile};
}

LoginRun LoginRun::gateway(std::string account,
                           std::string credential,
                           TimePoint deadline,
                           PollingProfile profile) {
    return LoginRun{LoginTarget::Gateway, std::move(account),
                    std::move(credential), deadline, profile};
}

std::optional<LoginRun> LoginRun::gateway_soak(
    std::string account,
    std::string credential,
    TimePoint deadline,
    PollingProfile profile,
    TimePoint hold_until) {
    if (hold_until < Clock::now()) {
        return std::nullopt;
    }
    return LoginRun{LoginTarget::GatewaySoak, std::move(account),
                    std::move(credential), deadline, profile, hold_until};
}

LoginRun LoginRun::full(std::string account,
                        std::string credential,
                        TimePoint deadline) {
    return LoginRun{LoginTarget::Full, std::move(account),
                    std::move(credential), deadline,
                    PollingProfile::ClientRealistic};
}

LoginRun LoginRun::full_for_loadgen(std::string account,
                                    std::string credential,
                                    TimePoint deadline,
                                    PollingProfile profile) {
    return LoginRun{LoginTarget::Full, std::move(account),
                    std::move(credential), deadline, profile};
}

LoginChain::LoginChain(LoginChainTransport& transport, LoginChainConfig config)
    : transport_(transport), config_(std::move(config)), poller_(config_.poll) {}

void LoginChain::wait_for(std::chrono::milliseconds duration,
                          TimePoint deadline) {
    const auto left = time_left(deadline);
    if (left.count() > 0) {
        std::this_thread::sleep_for(std::min(duration, left));
    }
}

bool LoginChain::within_admission_grant_window(TimePoint now) const {
    return now < admitted_at_ + admission_grant_ttl_;
}

LoginResult LoginChain::fail(LoginStage stage) const {
    return LoginResult{LoginFailure{stage, failure_, failure_detail_,
                                    credentials_.number, eta_}};
}

LoginChain::Action LoginChain::take_ticket(TimePoint deadline) {
    const auto result =
        transport_.take_ticket(credentials_.identity_token, deadline);
    if (!result.status.ok) {
        failure_ = result.status.failure;
        failure_detail_ = result.status.detail;
        return Action::Failed;
    }
    credentials_.queue_number_token = result.value.queue_number_token;
    credentials_.number = result.value.number;
    credentials_.admission_grant.clear();
    poller_.record_success();
    return Action::Advanced;
}

LoginChain::Action LoginChain::poll_until_admitted(TimePoint deadline) {
    std::uint64_t position = credentials_.number;

    for (bool first_query = true;; first_query = false) {
        if (Clock::now() >= deadline) {
            failure_ = ChainFailure::AdmitTimeout;
            failure_detail_ = "排队等待超出总窗口";
            return Action::Failed;
        }

        bool consult_ticket = false;
        if (first_query) {
            consult_ticket = true;
        } else {
            last_poll_interval_ =
                polling_profile_ == PollingProfile::Pressure
                ? config_.pressure_poll_interval
                : poller_.next_interval(position);
            wait_for(last_poll_interval_, deadline);
            if (Clock::now() >= deadline) {
                failure_ = ChainFailure::AdmitTimeout;
                failure_detail_ = "排队等待超出总窗口";
                return Action::Failed;
            }

            const auto progress = transport_.poll_progress(deadline);
            if (progress.status.ok) {
                poller_.record_success();
                const auto released = progress.value.released_number;
                position = credentials_.number > released
                    ? credentials_.number - released
                    : 0;
                eta_ = AdaptivePoller::estimate_eta(position,
                                                    progress.value.admit_rate);
                consult_ticket = released >= credentials_.number;
            } else {
                poller_.record_failure();
                consult_ticket = true;
            }
        }

        if (!consult_ticket) {
            continue;
        }

        const auto me =
            transport_.ticket_me(credentials_.queue_number_token, deadline);
        if (me.status.ok) {
            poller_.record_success();
            if (!me.value.admission_grant.empty()) {
                credentials_.admission_grant = me.value.admission_grant;
                admission_grant_ttl_ = me.value.admission_grant_ttl.count() > 0
                    ? me.value.admission_grant_ttl
                    : config_.admit_grace_fallback;
                admitted_at_ = Clock::now();
                failure_ = ChainFailure::None;
                failure_detail_.clear();
                set_stage(LoginStage::Admitted);
                return Action::Advanced;
            }
            position = me.value.position;
        } else if (me.status.recovery == PortRecovery::Restart) {
            failure_ = me.status.failure;
            failure_detail_ = me.status.detail;
            return Action::RestartLogin;
        } else {
            poller_.record_failure();
        }
    }
}

LoginChain::Action LoginChain::connect_gateway_and_handoff(
    TimePoint deadline,
    std::unique_ptr<GatewaySession>& session_out) {
    session_out.reset();
    for (;;) {
        if (Clock::now() >= deadline) {
            failure_ = ChainFailure::GatewayConnectFailed;
            failure_detail_ = "网关段超出总窗口";
            return Action::Failed;
        }
        set_stage(LoginStage::GatewayConnecting);

        bool restart_login = false;
        std::chrono::milliseconds retry_delay = config_.gateway_retry_delay;
        auto connected =
            transport_.connect_gateway(config_.gateway_endpoints, deadline);
        if (connected.status.ok && connected.value != nullptr) {
            auto session = std::move(connected.value);
            const auto& admission_grant = credentials_.admission_grant;
            const auto attached = transport_.attach(
                *session,
                credentials_.identity_token,
                admission_grant,
                deadline);
            if (attached.ok) {
                auto handoff = transport_.await_handoff(*session, deadline);
                if (handoff.status.ok) {
                    credentials_.enter_realm_ticket =
                        handoff.value.enter_realm_ticket;
                    realm_endpoints_ = std::move(handoff.value.realm_endpoints);
                    failure_ = ChainFailure::None;
                    failure_detail_.clear();
                    set_stage(LoginStage::HandoffReceived);
                    session_out = std::move(session);
                    return Action::Advanced;
                }
                if (handoff.status.recovery == PortRecovery::Restart) {
                    restart_login = true;
                } else {
                    failure_ = handoff.status.failure;
                    failure_detail_ = handoff.status.detail;
                    if (handoff.status.retry_after >
                        std::chrono::seconds::zero()) {
                        retry_delay = std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::min(handoff.status.retry_after,
                                     std::chrono::seconds{5}));
                    }
                }
            } else if (attached.recovery == PortRecovery::Restart) {
                restart_login = true;
            } else {
                failure_ = attached.failure == ChainFailure::None
                    ? ChainFailure::AttachRejected
                    : attached.failure;
                failure_detail_ = attached.detail;
                if (attached.retry_after > std::chrono::seconds::zero()) {
                    retry_delay = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        std::min(attached.retry_after,
                                 std::chrono::seconds{5}));
                }
            }
        } else {
            failure_ = connected.status.failure == ChainFailure::None
                ? ChainFailure::GatewayConnectFailed
                : connected.status.failure;
            failure_detail_ = connected.status.detail;
        }

        if (restart_login) {
            failure_ = ChainFailure::TicketRejected;
            failure_detail_ = "Admission Grant 无效,重新验证身份";
            return Action::RestartLogin;
        }
        if (!within_admission_grant_window(Clock::now())) {
            failure_ = ChainFailure::TicketRejected;
            failure_detail_ = "Admission Grant 窗口耗尽,重新验证身份";
            return Action::RestartLogin;
        }
        if (Clock::now() >= deadline) {
            return Action::Failed;
        }
        wait_for(retry_delay, deadline);
    }
}

LoginChain::Action LoginChain::redeem_realm(
    TimePoint deadline,
    std::unique_ptr<RealmSession>& session_out) {
    session_out.reset();
    const auto ticket_deadline =
        std::min(deadline, Clock::now() + config_.enter_realm_ttl);

    for (;;) {
        if (Clock::now() >= deadline) {
            failure_ = ChainFailure::RealmConnectFailed;
            failure_detail_ = "Realm 段超出总窗口";
            return Action::Failed;
        }
        set_stage(LoginStage::RealmConnecting);

        auto connected = transport_.connect_realm(realm_endpoints_, deadline);
        if (connected.status.ok && connected.value != nullptr) {
            auto session = std::move(connected.value);
            const auto redeemed = transport_.enter_realm(
                *session, credentials_.enter_realm_ticket, deadline);
            if (redeemed.ok) {
                failure_ = ChainFailure::None;
                failure_detail_.clear();
                set_stage(LoginStage::InGame);
                session_out = std::move(session);
                return Action::Advanced;
            }
            failure_ = redeemed.failure;
            failure_detail_ = redeemed.detail;
        } else {
            failure_ = connected.status.failure == ChainFailure::None
                ? ChainFailure::RealmConnectFailed
                : connected.status.failure;
            failure_detail_ = connected.status.detail;
        }

        if (Clock::now() >= ticket_deadline || Clock::now() >= deadline) {
            return Action::Failed;
        }
        wait_for(config_.realm_retry_delay, deadline);
    }
}

LoginResult LoginChain::run(LoginRun request) {
    credentials_ = ChainCredentials{};
    realm_endpoints_.clear();
    failure_ = ChainFailure::None;
    failure_detail_.clear();
    last_poll_interval_ = std::chrono::milliseconds{0};
    eta_.reset();
    admitted_at_ = TimePoint{};
    admission_grant_ttl_ = config_.admit_grace_fallback;
    polling_profile_ = request.polling_profile();

    const auto target = request.target();
    const auto deadline = request.deadline();
    if (Clock::now() >= deadline) {
        failure_ = ChainFailure::DeadlineExceeded;
        failure_detail_ = "登录总窗口已耗尽";
        set_stage(LoginStage::Idle);
        return fail(LoginStage::Idle);
    }

    for (;;) {
        set_stage(LoginStage::Verifying);
        const auto verified = transport_.verify(
            request.account(), request.credential(), deadline);
        if (!verified.status.ok) {
            failure_ = verified.status.failure;
            failure_detail_ = verified.status.detail;
            set_stage(LoginStage::Idle);
            return fail(LoginStage::Idle);
        }
        credentials_.identity_token = verified.value.identity_token;
        if (target == LoginTarget::Verify) {
            return LoginResult{LoginSuccess{
                VerifySuccess{credentials_.identity_token}}};
        }

        set_stage(LoginStage::Queued);
        if (take_ticket(deadline) == Action::Failed) {
            set_stage(LoginStage::Idle);
            return fail(LoginStage::Idle);
        }
        if (target == LoginTarget::Tickets) {
            return LoginResult{LoginSuccess{TicketsSuccess{
                credentials_.identity_token, credentials_.queue_number_token,
                credentials_.number}}};
        }

        const auto admitted = poll_until_admitted(deadline);
        if (admitted == Action::Failed) {
            set_stage(LoginStage::Idle);
            return fail(LoginStage::Idle);
        }
        if (admitted == Action::RestartLogin) {
            continue;
        }
        if (target == LoginTarget::Poll) {
            return LoginResult{LoginSuccess{PollSuccess{
                credentials_.admission_grant, credentials_.number, eta_}}};
        }

        bool restart_login = false;
        for (;;) {
            std::unique_ptr<GatewaySession> gateway_session;
            const auto gateway =
                connect_gateway_and_handoff(deadline, gateway_session);
            if (gateway == Action::Failed) {
                set_stage(LoginStage::Idle);
                return fail(LoginStage::Idle);
            }
            if (gateway == Action::RestartLogin) {
                restart_login = true;
                break;
            }

            HandoffResult handoff{credentials_.enter_realm_ticket,
                                  realm_endpoints_};
            if (target == LoginTarget::Gateway) {
                gateway_session.reset();
                return LoginResult{LoginSuccess{GatewaySuccess{
                    std::move(handoff), credentials_.number, eta_}}};
            }
            if (target == LoginTarget::GatewaySoak) {
                const auto held_until =
                    std::min(request.hold_until(), deadline);
                if (Clock::now() < held_until) {
                    std::this_thread::sleep_until(held_until);
                }
                gateway_session.reset();
                return LoginResult{LoginSuccess{GatewaySoakSuccess{
                    std::move(handoff), credentials_.number, eta_, held_until}}};
            }

            // Full 的 Realm 段独立竞速；Gateway 票据到手后即可释放。
            gateway_session.reset();
            std::unique_ptr<RealmSession> realm_session;
            const auto realm = redeem_realm(deadline, realm_session);
            if (realm == Action::Advanced) {
                return LoginResult{LoginSuccess{FullSuccess{
                    std::move(realm_session), credentials_.number, eta_}}};
            }
            if (realm == Action::RestartLogin) {
                restart_login = true;
                break;
            }
            if (Clock::now() >= deadline) {
                set_stage(LoginStage::Idle);
                return fail(LoginStage::Idle);
            }
            if (!within_admission_grant_window(Clock::now())) {
                restart_login = true;
                break;
            }
        }
        if (!restart_login) {
            return fail(stage_);
        }
    }
}

}  // namespace realm::client
