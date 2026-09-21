#pragma once

#include "realmmesh/client/adaptive_poller.hpp"
#include "realmmesh/client/login_stage.hpp"
#include "realmmesh/network/client/preferred_transport_connector.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace realm::client {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class PortFailureCategory { Protocol, Transport, Timeout };

/// Recovery selected by a transport operation. Retry keeps the current
/// credential chain; Restart discards Queue Number/Admission Grant and
/// starts again at Login Verifier.
enum class PortRecovery { Retry, Restart };

struct PortStatus final {
    bool ok{false};
    ChainFailure failure{ChainFailure::None};
    bool credential_expired{false};
    PortFailureCategory category{PortFailureCategory::Protocol};
    PortRecovery recovery{PortRecovery::Retry};
    std::chrono::seconds retry_after{0};
    std::string detail;

    [[nodiscard]] static PortStatus success();
    [[nodiscard]] static PortStatus error(
        ChainFailure failure,
        std::string detail = {},
        bool credential_expired = false,
        PortFailureCategory category = PortFailureCategory::Protocol,
        PortRecovery recovery = PortRecovery::Retry,
        std::chrono::seconds retry_after = std::chrono::seconds{0});
};

template <typename T>
struct PortValue final {
    PortStatus status;
    T value{};
};

struct VerifyResult final {
    std::string identity_token;
};

struct TicketResult final {
    std::string queue_number_token;
    std::uint64_t number{0};
};

struct ProgressResult final {
    std::uint64_t released_number{0};
    double admit_rate{0};
};

struct TicketMeResult final {
    std::uint64_t position{0};
    /// Empty while queued; populated only with the short-lived Admission
    /// Grant once the Queue Number's release window is open.
    std::string admission_grant;
    std::chrono::seconds admission_grant_ttl{0};
};

struct HandoffResult final {
    std::string enter_realm_ticket;
    std::vector<network::client::EndpointCandidate> realm_endpoints;
};

/// Gateway 与 Realm 的远程连接所有权。Transport 返回独占句柄；离开作用域
/// 即关闭，调用方不再承担配对 drop 调用。
class GatewaySession {
public:
    GatewaySession() = default;
    virtual ~GatewaySession() = default;
    GatewaySession(const GatewaySession&) = delete;
    GatewaySession& operator=(const GatewaySession&) = delete;
};

class RealmSession {
public:
    RealmSession() = default;
    virtual ~RealmSession() = default;
    RealmSession(const RealmSession&) = delete;
    RealmSession& operator=(const RealmSession&) = delete;
};

/// 登录链路唯一远程依赖 Seam。Session 参数把协议操作绑定到准确连接，
/// 成功与失败路径都由 RAII 保证清理。
class LoginChainTransport {
public:
    virtual ~LoginChainTransport() = default;

    virtual PortValue<VerifyResult> verify(
        std::string_view account,
        std::string_view credential,
        TimePoint deadline) = 0;
    virtual PortValue<TicketResult> take_ticket(
        std::string_view identity_token,
        TimePoint deadline) = 0;
    virtual PortValue<ProgressResult> poll_progress(TimePoint deadline) = 0;
    virtual PortValue<TicketMeResult> ticket_me(
        std::string_view queue_number_token,
        TimePoint deadline) = 0;

    virtual PortValue<std::unique_ptr<GatewaySession>> connect_gateway(
        std::span<const network::client::EndpointCandidate> candidates,
        TimePoint deadline) = 0;
    virtual PortStatus attach(
        GatewaySession& session,
        std::string_view identity_token,
        std::string_view admission_grant,
        TimePoint deadline) = 0;
    virtual PortValue<HandoffResult> await_handoff(
        GatewaySession& session,
        TimePoint deadline) = 0;

    virtual PortValue<std::unique_ptr<RealmSession>> connect_realm(
        std::span<const network::client::EndpointCandidate> candidates,
        TimePoint deadline) = 0;
    virtual PortStatus enter_realm(
        RealmSession& session,
        std::string_view enter_realm_ticket,
        TimePoint deadline) = 0;
};

struct LoginChainConfig final {
    PollTierConfig poll;
    std::vector<network::client::EndpointCandidate> gateway_endpoints;
    std::chrono::milliseconds gateway_retry_delay{200};
    std::chrono::milliseconds realm_retry_delay{200};
    std::chrono::seconds enter_realm_ttl{60};
    std::chrono::seconds admit_grace_fallback{300};
    /// 压测策略只替换轮询节奏，不改变状态、凭据或回退语义。
    std::chrono::milliseconds pressure_poll_interval{10};
};

enum class LoginTarget { Verify, Tickets, Poll, Gateway, GatewaySoak, Full };
enum class PollingProfile { ClientRealistic, Pressure };

/// 经过构造约束的一次运行。六个命名工厂杜绝 target 与数据字段的
/// 随意组合。
class LoginRun final {
public:
    [[nodiscard]] static LoginRun verify(
        std::string account, std::string credential, TimePoint deadline);
    [[nodiscard]] static LoginRun tickets(
        std::string account, std::string credential, TimePoint deadline);
    [[nodiscard]] static LoginRun poll(
        std::string account,
        std::string credential,
        TimePoint deadline,
        PollingProfile profile);
    [[nodiscard]] static LoginRun gateway(
        std::string account,
        std::string credential,
        TimePoint deadline,
        PollingProfile profile);
    [[nodiscard]] static std::optional<LoginRun> gateway_soak(
        std::string account,
        std::string credential,
        TimePoint deadline,
        PollingProfile profile,
        TimePoint hold_until);
    [[nodiscard]] static LoginRun full(
        std::string account, std::string credential, TimePoint deadline);
    [[nodiscard]] static LoginRun full_for_loadgen(
        std::string account,
        std::string credential,
        TimePoint deadline,
        PollingProfile profile);

    LoginRun(LoginRun&&) noexcept = default;
    LoginRun& operator=(LoginRun&&) noexcept = default;
    LoginRun(const LoginRun&) = delete;
    LoginRun& operator=(const LoginRun&) = delete;

    [[nodiscard]] LoginTarget target() const noexcept { return target_; }
    [[nodiscard]] PollingProfile polling_profile() const noexcept {
        return polling_profile_;
    }
    [[nodiscard]] std::string_view account() const noexcept { return account_; }
    [[nodiscard]] std::string_view credential() const noexcept {
        return credential_;
    }
    [[nodiscard]] TimePoint deadline() const noexcept { return deadline_; }
    [[nodiscard]] TimePoint hold_until() const noexcept { return hold_until_; }

private:
    LoginRun(LoginTarget target,
             std::string account,
             std::string credential,
             TimePoint deadline,
             PollingProfile profile,
             TimePoint hold_until = {});

    LoginTarget target_;
    std::string account_;
    std::string credential_;
    TimePoint deadline_;
    PollingProfile polling_profile_;
    TimePoint hold_until_{};
};

struct VerifySuccess final {
    std::string identity_token;
};
struct TicketsSuccess final {
    std::string identity_token;
    std::string queue_number_token;
    std::uint64_t number{0};
};
struct PollSuccess final {
    std::string admission_grant;
    std::uint64_t number{0};
    std::optional<std::chrono::seconds> eta;
};
struct GatewaySuccess final {
    HandoffResult handoff;
    std::uint64_t number{0};
    std::optional<std::chrono::seconds> eta;
};
struct GatewaySoakSuccess final {
    HandoffResult handoff;
    std::uint64_t number{0};
    std::optional<std::chrono::seconds> eta;
    TimePoint held_until;
};
struct FullSuccess final {
    std::unique_ptr<RealmSession> session;
    std::uint64_t number{0};
    std::optional<std::chrono::seconds> eta;
};

using LoginSuccess = std::variant<VerifySuccess,
                                  TicketsSuccess,
                                  PollSuccess,
                                  GatewaySuccess,
                                  GatewaySoakSuccess,
                                  FullSuccess>;

struct LoginFailure final {
    LoginStage stage{LoginStage::Idle};
    ChainFailure reason{ChainFailure::None};
    std::string detail;
    std::uint64_t number{0};
    std::optional<std::chrono::seconds> eta;
};

class LoginResult final {
public:
    LoginResult(LoginResult&&) noexcept = default;
    LoginResult& operator=(LoginResult&&) noexcept = default;
    LoginResult(const LoginResult&) = delete;
    LoginResult& operator=(const LoginResult&) = delete;

    [[nodiscard]] bool succeeded() const noexcept {
        return std::holds_alternative<LoginSuccess>(value_);
    }
    [[nodiscard]] const LoginSuccess* success() const noexcept {
        return std::get_if<LoginSuccess>(&value_);
    }
    [[nodiscard]] LoginSuccess* success() noexcept {
        return std::get_if<LoginSuccess>(&value_);
    }
    [[nodiscard]] const LoginFailure* failure() const noexcept {
        return std::get_if<LoginFailure>(&value_);
    }

private:
    friend class LoginChain;
    explicit LoginResult(LoginSuccess success) : value_(std::move(success)) {}
    explicit LoginResult(LoginFailure failure) : value_(std::move(failure)) {}

    std::variant<LoginSuccess, LoginFailure> value_;
};

/// 登录链路 Module：一个 run Interface，内部隐藏轮询、回退、竞速与
/// 会话转移。
class LoginChain final {
public:
    explicit LoginChain(
        LoginChainTransport& transport,
        LoginChainConfig config = {});

    [[nodiscard]] LoginResult run(LoginRun request);

    [[nodiscard]] std::chrono::milliseconds last_poll_interval() const
        noexcept {
        return last_poll_interval_;
    }

private:
    struct ChainCredentials final {
        std::string identity_token;
        std::string queue_number_token;
        std::uint64_t number{0};
        std::string admission_grant;
        std::string enter_realm_ticket;
    };
    enum class Action { Advanced, RestartLogin, Failed };

    [[nodiscard]] Action poll_until_admitted(TimePoint deadline);
    [[nodiscard]] Action connect_gateway_and_handoff(
        TimePoint deadline,
        std::unique_ptr<GatewaySession>& session_out);
    [[nodiscard]] Action redeem_realm(
        TimePoint deadline,
        std::unique_ptr<RealmSession>& session_out);
    [[nodiscard]] Action take_ticket(TimePoint deadline);

    [[nodiscard]] bool within_admission_grant_window(TimePoint now) const;
    [[nodiscard]] LoginResult fail(LoginStage stage) const;
    void set_stage(LoginStage stage) noexcept { stage_ = stage; }
    void wait_for(std::chrono::milliseconds duration, TimePoint deadline);

    LoginChainTransport& transport_;
    LoginChainConfig config_;
    AdaptivePoller poller_;
    ChainCredentials credentials_;
    LoginStage stage_{LoginStage::Idle};
    ChainFailure failure_{ChainFailure::None};
    std::string failure_detail_;
    std::chrono::milliseconds last_poll_interval_{0};
    std::optional<std::chrono::seconds> eta_;
    std::vector<network::client::EndpointCandidate> realm_endpoints_;
    TimePoint admitted_at_{};
    std::chrono::seconds admission_grant_ttl_{0};
    PollingProfile polling_profile_{PollingProfile::ClientRealistic};
};

}  // namespace realm::client
