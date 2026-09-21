#pragma once

// LoginChainTransport 的生产绑定:HTTP 段(verify/取号/轮询/查号)走
// TLS/TCP + HTTP/1.1(自研栈,ADR-0007),网关段与 Realm 段各自跑一次
// 「QUIC 主 + TLS/TCP 降级」竞速(0ms + 350ms 分级,ADR 见 0008)。
//
// 两段是同一条 edge 线(ALPN 与帧约定一致):网关段 1301/1302/1303,
// Realm 段 1304 兑换 / 1305 入场成功。信封编解码共用 game/common,帧
// 收发共用 network/client/EdgeClientConnection——不另起第二份线协议。

#include "realmmesh/client/login_chain.hpp"
#include "realmmesh/network/client/edge_client_connection.hpp"
#include "realmmesh/network/client/http1_client_connection.hpp"
#include "realmmesh/network/client/preferred_transport_connector.hpp"
#include "realmmesh/network/client/tls_tcp_client_dialer.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realm::client {

/// HTTP 段端点(登录健全服 / 排队调度服)。
struct WireEndpoints final {
    std::string login_verify_host{"127.0.0.1"};
    std::uint16_t login_verify_port{0};
    std::string queue_host{"127.0.0.1"};
    std::uint16_t queue_port{0};
    /// 校验服务端证书;自签部署/测试显式关闭。
    bool verify_peer{true};
};

struct WireTransportOptions final {
    /// 单次 HTTP 请求上限(与链路总窗口取小:一个挂死的请求不得吃光
    /// 整个登录窗口)。
    std::chrono::milliseconds request_timeout{10'000};
    /// 当前网络标识(竞速的 QUIC 负缓存按网络切换失效)。
    std::string network_id{"default"};
    /// 压测短连可选择释放即 RST；应用客户端保持默认优雅关闭。
    bool reset_close_on_release{false};
    /// 可选的 TLS 拨号失败诊断口；产品客户端默认不采集。
    network::client::TlsDialFailureObserver tls_dial_failure_observer;
    network::client::ConnectorOptions connector;
};

/// EnterRealm 兑换口:在已直连的 Realm 流上兑换 1303 下发的票据。
class EnterRealmRedeemer {
public:
    virtual ~EnterRealmRedeemer() = default;

    [[nodiscard]] virtual PortStatus redeem(
        network::client::ISecureByteStream& stream,
        std::string_view enter_realm_ticket,
        TimePoint deadline) = 0;
};

/// 生产兑换口:在已竞速建好的 Realm 流上发 1304 `EnterRealm` 帧、等 1305
/// `EnterRealmAccepted`。被 1999 `EdgeError` 拒(3002 无效票据、2002 未认证
/// 等)、坏帧或超时一律归 `ChainFailure::EnterRealmRejected`:链路在 Realm
/// 段没有「按错误码重取」的回退语义,细分只落在 detail 里。
class WireEnterRealmRedeemer final : public EnterRealmRedeemer {
public:
    [[nodiscard]] PortStatus redeem(
        network::client::ISecureByteStream& stream,
        std::string_view enter_realm_ticket,
        TimePoint deadline) override;
};

class WireLoginTransport final : public LoginChainTransport {
public:
    WireLoginTransport(WireEndpoints endpoints,
                       EnterRealmRedeemer& redeemer,
                       WireTransportOptions options = {});
    ~WireLoginTransport() override;
    WireLoginTransport(const WireLoginTransport&) = delete;
    WireLoginTransport& operator=(const WireLoginTransport&) = delete;

    [[nodiscard]] PortValue<VerifyResult> verify(
        std::string_view account,
        std::string_view credential,
        TimePoint deadline) override;
    [[nodiscard]] PortValue<TicketResult> take_ticket(
        std::string_view identity_token,
        TimePoint deadline) override;
    [[nodiscard]] PortValue<ProgressResult> poll_progress(
        TimePoint deadline) override;
    [[nodiscard]] PortValue<TicketMeResult> ticket_me(
        std::string_view queue_number_token,
        TimePoint deadline) override;

    [[nodiscard]] PortValue<std::unique_ptr<GatewaySession>> connect_gateway(
        std::span<const network::client::EndpointCandidate> candidates,
        TimePoint deadline) override;
    [[nodiscard]] PortStatus attach(
        GatewaySession& session,
        std::string_view identity_token,
        std::string_view admission_grant,
        TimePoint deadline) override;
    [[nodiscard]] PortValue<HandoffResult> await_handoff(
        GatewaySession& session,
        TimePoint deadline) override;

    [[nodiscard]] PortValue<std::unique_ptr<RealmSession>> connect_realm(
        std::span<const network::client::EndpointCandidate> candidates,
        TimePoint deadline) override;
    [[nodiscard]] PortStatus enter_realm(
        RealmSession& session,
        std::string_view enter_realm_ticket,
        TimePoint deadline) override;

private:
    /// 一次 JSON 请求;连接按需拨号 + 保活复用,网络层失败重拨一次。
    struct HttpResponse final {
        int status{0};
        std::string body;
    };
    /// HTTP 段的两台服务(各自独立保活连接)。
    enum class HttpSegment { LoginVerify, Queue };

    [[nodiscard]] std::optional<HttpResponse> call(
        HttpSegment segment,
        std::string_view method,
        std::string_view target,
        const std::optional<std::string>& bearer,
        std::string_view body,
        TimePoint deadline);

    /// 竞速的两段(网关段与 Realm 段各自独立:独立拨号器、独立负缓存)。
    enum class Segment { Gateway, Realm };

    [[nodiscard]] PortStatus connect_racing(
        Segment segment,
        std::span<const network::client::EndpointCandidate> candidates,
        std::shared_ptr<network::client::ISecureConnection>& connection_out,
        std::shared_ptr<network::client::ISecureByteStream>& stream_out,
        TimePoint deadline);

    WireEndpoints endpoints_;
    EnterRealmRedeemer& redeemer_;
    WireTransportOptions options_;
    network::client::TlsClientOptions tls_options_;
    network::client::TlsTcpClientDialer gateway_dialer_;
    network::client::TlsTcpClientDialer realm_dialer_;
    network::client::PreferredTransportConnector gateway_connector_;
    network::client::PreferredTransportConnector realm_connector_;

    std::unique_ptr<network::client::Http1ClientConnection> login_connection_;
    std::unique_ptr<network::client::Http1ClientConnection> queue_connection_;
};

}  // namespace realm::client
