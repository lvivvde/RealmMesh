#pragma once

#include "realmmesh/client/adaptive_poller.hpp"
#include "realmmesh/client/login_stage.hpp"
#include "realmmesh/network/client/preferred_transport_connector.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace realm::client {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

/// 端口调用状态:失败必带分型。credential_expired 表示服务端判定
/// 号牌/票据过期(触发自动重取),与瞬时网络失败区分(spec §7 回退规则)。
struct PortStatus final {
    bool ok{false};
    ChainFailure failure{ChainFailure::None};
    bool credential_expired{false};
    std::string detail;

    [[nodiscard]] static PortStatus success();
    [[nodiscard]] static PortStatus error(
        ChainFailure failure,
        std::string detail = {},
        bool credential_expired = false);
};

template <typename T>
struct PortValue final {
    PortStatus status;
    T value{};
};

struct VerifyResult final {
    std::string identity_token;
    std::string account_id;
};

struct TicketResult final {
    std::string queue_number_token;
    std::uint64_t number{0};
    std::chrono::seconds estimated_wait{0};
};

struct ProgressResult final {
    std::uint64_t released_number{0};
    double admit_rate{0};
};

struct TicketMeResult final {
    bool admitted{false};
    std::uint64_t position{0};
    /// admitted 时的重签号牌(admit_grant)。
    std::string admitted_token;
    /// admitted 后的宽限(spec §4:放行 + 5 min 宽限)。
    std::chrono::seconds admit_grace{0};
};

struct HandoffResult final {
    std::string enter_realm_ticket;
    /// 1303 下发的业务服端点(protocol + priority 已就位,直接喂竞速)。
    std::vector<network::client::EndpointCandidate> realm_endpoints;
};

/// 链路传输口:唯一的注入缝。生产绑定走自研 HTTP/1.1 与 edge 帧(ADR-0007),
/// 测试以假实现脚本化。会话型:connect_* 建连并持有,attach/
/// await_handoff/enter_realm 作用于当前会话,drop_connections 清理回退。
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

    /// 连网关:竞速候选(QUIC + TLS/TCP),建连成功即持有会话。
    virtual PortStatus connect_gateway(
        std::span<const network::client::EndpointCandidate> candidates,
        TimePoint deadline) = 0;
    virtual PortStatus attach(
        std::string_view identity_token,
        std::string_view queue_number_token,
        TimePoint deadline) = 0;
    virtual PortValue<HandoffResult> await_handoff(TimePoint deadline) = 0;

    /// 直连业务服:与网关段各自独立的第二次竞速。
    virtual PortStatus connect_realm(
        std::span<const network::client::EndpointCandidate> candidates,
        TimePoint deadline) = 0;
    virtual PortStatus enter_realm(
        std::string_view enter_realm_ticket,
        TimePoint deadline) = 0;

    virtual void drop_connections() = 0;
};

struct LoginChainConfig final {
    PollTierConfig poll;
    /// 网关竞速候选(部署侧注入;QUIC/TLS 两项各带优先级)。
    std::vector<network::client::EndpointCandidate> gateway_endpoints;
    /// 宽限内重入节奏(网关段/Realm 段各自独立)。
    std::chrono::milliseconds gateway_retry_delay{200};
    std::chrono::milliseconds realm_retry_delay{200};
    /// EnterRealm 票据 TTL(spec §4:60s);票据过期回网关重入或重取号。
    std::chrono::seconds enter_realm_ttl{60};
    /// tickets/me 未给宽限时的兜底(规格:放行 + 5 min)。
    std::chrono::seconds admit_grace_fallback{300};
};

/// 凭据包:纯内存持有(spec §7:无落盘、无序列化;进程重启=重新登录排队)。
struct ChainCredentials final {
    std::string identity_token;
    std::string queue_number_token;
    std::uint64_t number{0};
    std::string admitted_token;
    std::string enter_realm_ticket;
};

struct LoginChainResult final {
    LoginStage stage{LoginStage::Idle};
    ChainFailure failure{ChainFailure::None};
    std::string detail;
    std::uint64_t number{0};
    std::optional<std::chrono::seconds> eta;

    [[nodiscard]] bool succeeded() const noexcept {
        return stage == LoginStage::InGame;
    }
};

/// 七态登录链路(spec §7):阻塞式单线程编排,从 Idle 走到 InGame,
/// 或按回退规则回到 Idle/Queued(结果里带状态与失败分型)。
class LoginChain final {
public:
    explicit LoginChain(
        LoginChainTransport& transport,
        LoginChainConfig config = {});

    /// 驱动一次完整登录;deadline 是总窗口(含排队等待)。
    LoginChainResult run(
        std::string_view account,
        std::string_view credential,
        TimePoint deadline);

    [[nodiscard]] LoginStage stage() const noexcept { return stage_; }
    [[nodiscard]] const ChainCredentials& credentials() const noexcept {
        return credentials_;
    }
    /// 最后一次排队轮询实际等待的间隔(可观测:分档/退避的外部表征)。
    [[nodiscard]] std::chrono::milliseconds last_poll_interval() const
        noexcept {
        return last_poll_interval_;
    }
    void on_stage_change(std::function<void(LoginStage)> callback);

private:
    /// 子阶段结局:推进 / 号牌过期需重取 / 窗口内失败。
    enum class Step { Advanced, RetakeTicket, Failed };

    [[nodiscard]] Step poll_until_admitted(TimePoint deadline);
    [[nodiscard]] Step connect_gateway_and_handoff(TimePoint deadline);
    [[nodiscard]] Step redeem_realm(TimePoint deadline);
    [[nodiscard]] Step take_ticket(TimePoint deadline);

    [[nodiscard]] bool within_admit_grace(TimePoint now) const;
    [[nodiscard]] LoginChainResult finish(LoginStage stage) const;
    void set_stage(LoginStage stage);
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
    /// 1303 下发的业务服端点,供 Realm 段竞速使用。
    std::vector<network::client::EndpointCandidate> realm_endpoints_;
    TimePoint admitted_at_{};
    std::chrono::seconds admit_grace_{0};
    std::function<void(LoginStage)> on_stage_change_;
};

}  // namespace realm::client
