#include "realmmesh/client/login_chain.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace realm::client {
namespace {

/// 距截止的剩余时间;已过期返回 0(调用方据此判窗口耗尽)。
[[nodiscard]] std::chrono::milliseconds time_left(TimePoint deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(
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
                             bool credential_expired) {
    PortStatus status;
    status.ok = false;
    status.failure = failure;
    status.credential_expired = credential_expired;
    status.detail = std::move(detail);
    return status;
}

LoginChain::LoginChain(LoginChainTransport& transport, LoginChainConfig config)
    : transport_(transport), config_(std::move(config)),
      poller_(config_.poll) {}

void LoginChain::on_stage_change(std::function<void(LoginStage)> callback) {
    on_stage_change_ = std::move(callback);
}

void LoginChain::set_stage(LoginStage stage) {
    stage_ = stage;
    if (on_stage_change_) {
        on_stage_change_(stage);
    }
}

void LoginChain::wait_for(std::chrono::milliseconds duration,
                          TimePoint deadline) {
    const auto left = time_left(deadline);
    if (left.count() <= 0) {
        return;
    }
    std::this_thread::sleep_for(std::min(duration, left));
}

bool LoginChain::within_admit_grace(TimePoint now) const {
    // 宽限从收到放行时刻起算(spec §4:放行 + 5 min)。
    return now < admitted_at_ + admit_grace_;
}

LoginChainResult LoginChain::finish(LoginStage stage) const {
    return LoginChainResult{stage, failure_, failure_detail_,
                            credentials_.number, eta_};
}

LoginChain::Action LoginChain::take_ticket(TimePoint deadline) {
    const auto result =
        transport_.take_ticket(credentials_.identity_token, deadline);
    if (!result.status.ok) {
        failure_ = result.status.failure;
        failure_detail_ = result.status.detail;
        return Action::Failed;
    }
    // 重取号等于新排队:凭据与轮询节奏一并复位。
    credentials_.queue_number_token = result.value.queue_number_token;
    credentials_.number = result.value.number;
    credentials_.admitted_token.clear();
    poller_.record_success();
    return Action::Advanced;
}

LoginChain::Action LoginChain::poll_until_admitted(TimePoint deadline) {
    // 无 progress 信息前的保守位次:就是自己手上的号。
    std::uint64_t position = credentials_.number;

    for (bool first_query = true;; first_query = false) {
        if (Clock::now() >= deadline) {
            failure_ = ChainFailure::AdmitTimeout;
            failure_detail_ = "排队等待超出总窗口";
            return Action::Failed;
        }

        bool consult_ticket = false;
        if (first_query) {
            // 首查 tickets/me(spec §5.1 #4):取号响应里的位次只是签发时
            // 的估算;且号值可能已进放行区间,首查能立刻拿到放行凭证,
            // 不必先白等一个轮询间隔。
            consult_ticket = true;
        } else {
            last_poll_interval_ = poller_.next_interval(position);
            wait_for(last_poll_interval_, deadline);
            if (Clock::now() >= deadline) {
                failure_ = ChainFailure::AdmitTimeout;
                failure_detail_ = "排队等待超出总窗口";
                return Action::Failed;
            }

            // 主查询:全局 progress(单调可缓存,ADR-0006)。
            const auto progress = transport_.poll_progress(deadline);
            if (progress.status.ok) {
                poller_.record_success();
                const auto released = progress.value.released_number;
                position = credentials_.number > released
                    ? credentials_.number - released
                    : 0;
                eta_ = AdaptivePoller::estimate_eta(position,
                                                    progress.value.admit_rate);
                // 号值已进放行区间:放行是分批的,去 tickets/me 取准信
                // (兜底,spec §5.1 #4)。
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
            if (me.value.admitted) {
                credentials_.admitted_token = me.value.admitted_token;
                admit_grace_ = me.value.admit_grace.count() > 0
                    ? me.value.admit_grace
                    : config_.admit_grace_fallback;
                admitted_at_ = Clock::now();
                failure_ = ChainFailure::None;
                failure_detail_.clear();
                set_stage(LoginStage::Admitted);
                return Action::Advanced;
            }
            position = me.value.position;
        } else if (me.status.credential_expired) {
            // 号牌过期 → 自动重取(spec §7)。
            failure_ = me.status.failure;
            failure_detail_ = me.status.detail;
            return Action::RetakeTicket;
        } else {
            poller_.record_failure();
        }
    }
}

LoginChain::Action LoginChain::connect_gateway_and_handoff(TimePoint deadline) {
    for (;;) {
        if (Clock::now() >= deadline) {
            failure_ = ChainFailure::GatewayConnectFailed;
            failure_detail_ = "网关段超出总窗口";
            return Action::Failed;
        }
        set_stage(LoginStage::GatewayConnecting);

        // 号牌被网关判过期:本段不再重试,释放会话后回排队重取。
        bool retake_ticket = false;
        const auto connected =
            transport_.connect_gateway(config_.gateway_endpoints, deadline);
        if (connected.ok) {
            const auto& queue_token = credentials_.admitted_token.empty()
                ? credentials_.queue_number_token
                : credentials_.admitted_token;
            const auto attached = transport_.attach(
                credentials_.identity_token, queue_token, deadline);
            if (attached.ok) {
                const auto handoff = transport_.await_handoff(deadline);
                if (handoff.status.ok) {
                    credentials_.enter_realm_ticket =
                        handoff.value.enter_realm_ticket;
                    realm_endpoints_ = handoff.value.realm_endpoints;
                    failure_ = ChainFailure::None;
                    failure_detail_.clear();
                    // 票据已在手:网关会话不再需要,立即释放(Realm 段
                    // 自己拨号;若 Realm 段失败回网关,重入会重新建连)。
                    transport_.drop_connections();
                    set_stage(LoginStage::HandoffReceived);
                    return Action::Advanced;
                }
                if (handoff.status.credential_expired) {
                    retake_ticket = true;
                } else {
                    failure_ = handoff.status.failure;
                    failure_detail_ = handoff.status.detail;
                }
            } else if (attached.credential_expired) {
                retake_ticket = true;
            } else {
                failure_ = ChainFailure::AttachRejected;
                failure_detail_ = attached.detail;
            }
        } else {
            failure_ = ChainFailure::GatewayConnectFailed;
            failure_detail_ = connected.detail;
        }

        // 会话一律释放:重取号、宽限内重入、退出三条路都不留悬挂的
        // Edge Session(回排队后本段建的连接没有任何复用价值)。
        transport_.drop_connections();
        if (retake_ticket) {
            failure_ = ChainFailure::TicketRejected;
            failure_detail_ = "号牌被网关判过期,回排队重取";
            return Action::RetakeTicket;
        }

        // 回退:admitted 号牌在宽限内可重入;宽限耗尽即视为号牌过期,
        // 回排队阶段自动重取(spec §7)。
        if (!within_admit_grace(Clock::now())) {
            failure_ = ChainFailure::TicketRejected;
            failure_detail_ = "放行宽限耗尽,号牌需重取";
            return Action::RetakeTicket;
        }
        if (Clock::now() >= deadline) {
            return Action::Failed;
        }
        wait_for(config_.gateway_retry_delay, deadline);
    }
}

LoginChain::Action LoginChain::redeem_realm(TimePoint deadline) {
    // EnterRealm 票据 60s 是本段的硬窗(spec §4/§7)。
    const auto ticket_deadline =
        std::min(deadline, Clock::now() + config_.enter_realm_ttl);

    for (;;) {
        if (Clock::now() >= deadline) {
            failure_ = ChainFailure::RealmConnectFailed;
            failure_detail_ = "Realm 段超出总窗口";
            return Action::Failed;
        }
        set_stage(LoginStage::RealmConnecting);

        const auto connected =
            transport_.connect_realm(realm_endpoints_, deadline);
        if (connected.ok) {
            const auto redeemed =
                transport_.enter_realm(credentials_.enter_realm_ticket,
                                       deadline);
            if (redeemed.ok) {
                failure_ = ChainFailure::None;
                failure_detail_.clear();
                set_stage(LoginStage::InGame);
                return Action::Advanced;
            }
            failure_ = redeemed.failure;
            failure_detail_ = redeemed.detail;
        } else {
            failure_ = ChainFailure::RealmConnectFailed;
            failure_detail_ = connected.detail;
        }

        transport_.drop_connections();
        if (Clock::now() >= ticket_deadline) {
            // 票据窗口耗尽:交回上层决定回网关重入还是重取号。
            return Action::Failed;
        }
        if (Clock::now() >= deadline) {
            return Action::Failed;
        }
        wait_for(config_.realm_retry_delay, deadline);
    }
}

LoginChainResult LoginChain::run(std::string_view account,
                                 std::string_view credential,
                                 TimePoint deadline) {
    credentials_ = ChainCredentials{};
    realm_endpoints_.clear();
    failure_ = ChainFailure::None;
    failure_detail_.clear();
    last_poll_interval_ = std::chrono::milliseconds{0};
    eta_.reset();
    admitted_at_ = TimePoint{};
    admit_grace_ = config_.admit_grace_fallback;

    // verifying:失败即回 idle(spec §7)。
    set_stage(LoginStage::Verifying);
    const auto verified = transport_.verify(account, credential, deadline);
    if (!verified.status.ok) {
        failure_ = verified.status.failure;
        failure_detail_ = verified.status.detail;
        set_stage(LoginStage::Idle);
        return finish(LoginStage::Idle);
    }
    credentials_.identity_token = verified.value.identity_token;

    // queued 起的循环:任何"重取号"回退都从头再来一轮排队。
    for (;;) {
        set_stage(LoginStage::Queued);
        if (take_ticket(deadline) == Action::Failed) {
            set_stage(LoginStage::Idle);
            return finish(LoginStage::Idle);
        }

        // queued → admitted(含号牌过期自动重取)。
        const auto admitted = poll_until_admitted(deadline);
        if (admitted == Action::Failed) {
            set_stage(LoginStage::Idle);
            return finish(LoginStage::Idle);
        }
        if (admitted == Action::RetakeTicket) {
            continue;
        }

        // 同一号牌内的网关段与 Realm 段:Realm 段失败且宽限仍在时,
        // 回网关重入换新票据;宽限耗尽即弃号回排队(spec §7)。
        bool retake_ticket = false;
        for (;;) {
            const auto gateway = connect_gateway_and_handoff(deadline);
            if (gateway == Action::Failed) {
                set_stage(LoginStage::Idle);
                return finish(LoginStage::Idle);
            }
            if (gateway == Action::RetakeTicket) {
                retake_ticket = true;
                break;
            }

            const auto realm = redeem_realm(deadline);
            if (realm == Action::Advanced) {
                return finish(LoginStage::InGame);
            }
            if (realm == Action::RetakeTicket) {
                retake_ticket = true;
                break;
            }
            if (Clock::now() >= deadline) {
                set_stage(LoginStage::Idle);
                return finish(LoginStage::Idle);
            }
            if (!within_admit_grace(Clock::now())) {
                retake_ticket = true;
                break;
            }
        }
        if (!retake_ticket) {
            // 内层只在 return 或置位 retake_ticket 后退出,这里仅作兜底。
            return finish(stage_);
        }
    }
}

}  // namespace realm::client
