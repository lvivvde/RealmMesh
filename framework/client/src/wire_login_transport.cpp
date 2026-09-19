#include "realmmesh/client/wire_login_transport.hpp"

#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/network/client/json_field.hpp"
#include "realmmesh/network/transport/transport_config.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace realm::client {
namespace {

namespace common = ::realm::game::common;
namespace edge_v1 = ::realmmesh::protocol::edge::v1;
namespace net_client = ::realm::network::client;

/// 单次请求上限与链路总窗口取小:一个挂死的请求不得吃光整个登录窗口。
[[nodiscard]] TimePoint request_deadline(TimePoint deadline,
                                        std::chrono::milliseconds budget) {
    return std::min(deadline, Clock::now() + budget);
}

[[nodiscard]] std::string connect_failure_detail(
    net_client::ConnectFailure failure) {
    switch (failure) {
        case net_client::ConnectFailure::Unsupported:
            return "候选传输不受支持";
        case net_client::ConnectFailure::NetworkUnreachable:
            return "网络不可达";
        case net_client::ConnectFailure::HandshakeTimeout:
            return "握手超时";
        case net_client::ConnectFailure::CertificateRejected:
            return "证书被拒";
        case net_client::ConnectFailure::AlpnRejected:
            return "ALPN 协商失败";
        case net_client::ConnectFailure::AuthenticationRejected:
            return "认证被拒";
        case net_client::ConnectFailure::ProtocolError:
            return "协议错误";
        case net_client::ConnectFailure::Cancelled:
            return "竞速取消";
        case net_client::ConnectFailure::NoCandidate:
            return "无可用候选端点";
    }
    return "未知连接失败";
}

/// 1303 下发的业务服端点 → 竞速候选(协议与优先级原样带过)。
[[nodiscard]] net_client::EndpointCandidate to_endpoint_candidate(
    const common::ServiceEndpoint& endpoint) {
    net_client::EndpointCandidate candidate;
    candidate.protocol =
        endpoint.protocol() == edge_v1::TRANSPORT_PROTOCOL_QUIC
            ? ::realm::network::TransportProtocol::Quic
            : ::realm::network::TransportProtocol::TlsTcp;
    candidate.host = endpoint.address();
    candidate.port = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(endpoint.port(), 0xFFFFU));
    candidate.priority = endpoint.priority();
    return candidate;
}

[[nodiscard]] std::string edge_error_detail(const common::EdgeError& error) {
    return "edge error " + std::to_string(error.code()) + ": " +
           error.message();
}

class WireGatewaySession final : public GatewaySession {
public:
    WireGatewaySession(
        std::shared_ptr<net_client::ISecureConnection> connection,
        std::shared_ptr<net_client::ISecureByteStream> stream)
        : connection_(std::move(connection)), stream_(std::move(stream)),
          edge_(stream_) {}

    ~WireGatewaySession() override {
        if (stream_ != nullptr) {
            stream_->shutdown();
        }
    }

    net_client::EdgeClientConnection& edge() noexcept { return edge_; }

private:
    std::shared_ptr<net_client::ISecureConnection> connection_;
    std::shared_ptr<net_client::ISecureByteStream> stream_;
    net_client::EdgeClientConnection edge_;
};

class WireRealmSession final : public RealmSession {
public:
    WireRealmSession(
        std::shared_ptr<net_client::ISecureConnection> connection,
        std::shared_ptr<net_client::ISecureByteStream> stream)
        : connection_(std::move(connection)), stream_(std::move(stream)) {}

    ~WireRealmSession() override {
        if (stream_ != nullptr) {
            stream_->shutdown();
        }
    }

    net_client::ISecureByteStream& stream() noexcept { return *stream_; }

private:
    std::shared_ptr<net_client::ISecureConnection> connection_;
    std::shared_ptr<net_client::ISecureByteStream> stream_;
};

}  // namespace

PortStatus WireEnterRealmRedeemer::redeem(
    net_client::ISecureByteStream& stream,
    std::string_view enter_realm_ticket,
    TimePoint deadline) {
    // 借用竞速胜出的 Realm 流:兑换口不接管所有权,流由链路持有到窗口结束。
    net_client::EdgeClientConnection edge(stream);
    common::EnterRealm request;
    request.set_enter_realm_ticket(std::string{enter_realm_ticket});
    // request_id 取 0:Realm 流上只跑这一个请求,服务端按 0 回执。
    if (!edge.send_frame(common::encode(request, 0), deadline)) {
        return PortStatus::error(ChainFailure::EnterRealmRejected,
                                 "EnterRealm 帧发送失败", false,
                                 PortFailureCategory::Transport);
    }
    for (;;) {
        const auto payload = edge.receive_frame(deadline);
        if (!payload.has_value()) {
            return PortStatus::error(ChainFailure::EnterRealmRejected,
                                     "未在窗口内收到入场回执", false,
                                     PortFailureCategory::Timeout);
        }
        const auto message_id = common::edge_message_id(*payload);
        if (!message_id.has_value()) {
            return PortStatus::error(ChainFailure::EnterRealmRejected, "坏帧");
        }
        if (*message_id == edge_v1::MESSAGE_ID_S2C_ENTER_REALM_ACCEPTED) {
            if (!common::decode_enter_realm_accepted(*payload).has_value()) {
                return PortStatus::error(ChainFailure::EnterRealmRejected,
                                         "坏帧");
            }
            return PortStatus::success();
        }
        if (*message_id == edge_v1::MESSAGE_ID_S2C_ERROR) {
            const auto error = common::decode_edge_error(*payload);
            // 不按错误码分支:Realm 段没有「按码重取」的回退语义,码只进 detail。
            return PortStatus::error(
                ChainFailure::EnterRealmRejected,
                error.has_value() ? edge_error_detail(*error) : "入场被拒");
        }
        // 其余帧(不应出现):忽略继续等入场回执。
    }
}

WireLoginTransport::WireLoginTransport(WireEndpoints endpoints,
                                       EnterRealmRedeemer& redeemer,
                                       WireTransportOptions options)
    : endpoints_(std::move(endpoints)), redeemer_(redeemer),
      options_(std::move(options)),
      tls_options_{.verify_peer = endpoints_.verify_peer,
                   .reset_close_on_release =
                       options_.reset_close_on_release},
      gateway_dialer_(std::string{::realm::network::kEdgeAlpn},
                      endpoints_.verify_peer,
                      options_.reset_close_on_release),
      realm_dialer_(std::string{::realm::network::kEdgeAlpn},
                    endpoints_.verify_peer,
                    options_.reset_close_on_release),
      gateway_connector_(gateway_dialer_, options_.connector),
      realm_connector_(realm_dialer_, options_.connector) {}

WireLoginTransport::~WireLoginTransport() = default;

std::optional<WireLoginTransport::HttpResponse> WireLoginTransport::call(
    HttpSegment segment,
    std::string_view method,
    std::string_view target,
    const std::optional<std::string>& bearer,
    std::string_view body,
    TimePoint deadline) {
    const bool queue = segment == HttpSegment::Queue;
    auto& connection = queue ? queue_connection_ : login_connection_;
    const std::string& host =
        queue ? endpoints_.queue_host : endpoints_.login_verify_host;
    const std::uint16_t port =
        queue ? endpoints_.queue_port : endpoints_.login_verify_port;

    // 一次正常路径 + 一次「连接作废后重拨」:保活连接被对端回收是常态。
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (connection == nullptr) {
            auto dial = net_client::Http1ClientConnection::dial(
                host, port, tls_options_,
                request_deadline(deadline, options_.request_timeout));
            if (!dial.ok()) {
                return std::nullopt;
            }
            connection = std::make_unique<net_client::Http1ClientConnection>(
                std::move(dial.stream));
        }
        auto response = connection->request(
            method, target, host, bearer, body,
            request_deadline(deadline, options_.request_timeout));
        if (response.has_value()) {
            return HttpResponse{response->status, std::move(response->body)};
        }
        connection.reset();
    }
    return std::nullopt;
}

PortValue<VerifyResult> WireLoginTransport::verify(
    std::string_view account,
    std::string_view credential,
    TimePoint deadline) {
    PortValue<VerifyResult> result;
    const std::string body = "{\"account\":\"" + std::string{account} +
                             "\",\"credential\":\"" +
                             std::string{credential} + "\"}";
    const auto response = call(HttpSegment::LoginVerify, "POST",
                               "/v1/login/verify", std::nullopt, body, deadline);
    if (!response.has_value()) {
        result.status = PortStatus::error(
            ChainFailure::VerifyRejected, "健全服请求失败", false,
            PortFailureCategory::Transport);
        return result;
    }
    const auto token =
        net_client::extract_json_string_field(response->body, "identity_token");
    if (response->status != 200 || !token.has_value()) {
        result.status = PortStatus::error(
            ChainFailure::VerifyRejected,
            "健全服拒绝: http_status=" + std::to_string(response->status));
        return result;
    }
    result.value.identity_token = *token;
    result.status = PortStatus::success();
    return result;
}

PortValue<TicketResult> WireLoginTransport::take_ticket(
    std::string_view identity_token,
    TimePoint deadline) {
    PortValue<TicketResult> result;
    const auto response = call(HttpSegment::Queue, "POST", "/v1/queue/tickets",
                               std::string{identity_token}, "", deadline);
    if (!response.has_value()) {
        result.status = PortStatus::error(
            ChainFailure::TicketRejected, "排队服请求失败", false,
            PortFailureCategory::Transport);
        return result;
    }
    const auto token = net_client::extract_json_string_field(
        response->body, "queue_number_token");
    const auto number =
        net_client::extract_json_int_field(response->body, "number");
    if (response->status != 202 || !token.has_value() || !number.has_value() ||
        *number < 0) {
        result.status = PortStatus::error(
            ChainFailure::TicketRejected,
            "取号被拒: http_status=" + std::to_string(response->status));
        return result;
    }
    result.value.queue_number_token = *token;
    result.value.number = static_cast<std::uint64_t>(*number);
    result.status = PortStatus::success();
    return result;
}

PortValue<ProgressResult> WireLoginTransport::poll_progress(
    TimePoint deadline) {
    PortValue<ProgressResult> result;
    const auto response = call(HttpSegment::Queue, "GET", "/v1/queue/progress",
                               std::nullopt, "", deadline);
    if (!response.has_value()) {
        result.status = PortStatus::error(
            ChainFailure::ProgressFailed, "进度请求失败", false,
            PortFailureCategory::Transport);
        return result;
    }
    const auto released =
        net_client::extract_json_int_field(response->body, "released_number");
    if (response->status != 200 || !released.has_value() || *released < 0) {
        result.status = PortStatus::error(
            ChainFailure::ProgressFailed,
            "进度响应畸形: http_status=" + std::to_string(response->status));
        return result;
    }
    result.value.released_number = static_cast<std::uint64_t>(*released);
    if (const auto rate =
            net_client::extract_json_int_field(response->body, "admit_rate");
        rate.has_value() && *rate > 0) {
        result.value.admit_rate = static_cast<double>(*rate);
    }
    result.status = PortStatus::success();
    return result;
}

PortValue<TicketMeResult> WireLoginTransport::ticket_me(
    std::string_view queue_number_token,
    TimePoint deadline) {
    PortValue<TicketMeResult> result;
    const auto response = call(HttpSegment::Queue, "GET",
                               "/v1/queue/tickets/me",
                               std::string{queue_number_token}, "", deadline);
    if (!response.has_value()) {
        result.status = PortStatus::error(
            ChainFailure::ProgressFailed, "查号请求失败", false,
            PortFailureCategory::Transport);
        return result;
    }
    const auto status =
        net_client::extract_json_string_field(response->body, "status");
    // 401 不一律等于号牌过期:只有错误码 2001(invalid number,spec §5.1
    // 错误模型)才是「号牌无效/过期」→ 上层自动重取(spec §7)。其余
    // 401 按瞬时失败处理(可由下一次轮询重试),不能误触发重取号。
    if (response->status == 401) {
        const auto code =
            net_client::extract_json_int_field(response->body, "code");
        if (code.has_value() &&
            *code == static_cast<std::int64_t>(
                         common::edge_error_invalid_queue_number)) {
            result.status = PortStatus::error(
                ChainFailure::TicketRejected, "号牌无效或已过期",
                /*credential_expired=*/true);
            return result;
        }
        result.status = PortStatus::error(
            ChainFailure::ProgressFailed,
            "查号被拒: http_status=401 code=" +
                (code.has_value() ? std::to_string(*code) : std::string{"?"}));
        return result;
    }
    if (response->status != 200 || !status.has_value()) {
        result.status = PortStatus::error(
            ChainFailure::ProgressFailed,
            "查号响应畸形: http_status=" + std::to_string(response->status));
        return result;
    }
    if (*status == "admitted") {
        // 放行凭证嵌在 admit_grant 内(服务端只编扁平对象的拼接体),是
        // 响应体首个 queue_number_token;expires_in 即宽限秒数(spec §4)。
        const auto grant = net_client::extract_json_string_field(
            response->body, "queue_number_token");
        if (!grant.has_value() || grant->empty()) {
            result.status = PortStatus::error(ChainFailure::ProgressFailed,
                                              "放行响应缺号牌重签");
            return result;
        }
        result.value.admitted = true;
        result.value.admitted_token = *grant;
        if (const auto grace =
                net_client::extract_json_int_field(response->body, "expires_in");
            grace.has_value() && *grace > 0) {
            result.value.admit_grace = std::chrono::seconds{*grace};
        }
        result.status = PortStatus::success();
        return result;
    }
    if (const auto position =
            net_client::extract_json_int_field(response->body, "position");
        position.has_value() && *position >= 0) {
        result.value.position = static_cast<std::uint64_t>(*position);
    }
    result.status = PortStatus::success();
    return result;
}

PortStatus WireLoginTransport::connect_racing(
    Segment segment,
    std::span<const net_client::EndpointCandidate> candidates,
    std::shared_ptr<net_client::ISecureConnection>& connection_out,
    std::shared_ptr<net_client::ISecureByteStream>& stream_out,
    TimePoint deadline) {
    const bool gateway = segment == Segment::Gateway;
    const auto failure = gateway ? ChainFailure::GatewayConnectFailed
                                 : ChainFailure::RealmConnectFailed;
    if (Clock::now() >= deadline) {
        return PortStatus::error(ChainFailure::DeadlineExceeded,
                                 "窗口内无建连预算", false,
                                 PortFailureCategory::Timeout);
    }
    auto& connector = gateway ? gateway_connector_ : realm_connector_;
    auto attempt = connector.connect(candidates, options_.network_id);
    if (const auto* cause = std::get_if<net_client::ConnectFailure>(&attempt);
        cause != nullptr) {
        return PortStatus::error(failure, connect_failure_detail(*cause),
                                 false, PortFailureCategory::Transport);
    }
    auto connection =
        std::get<std::shared_ptr<net_client::ISecureConnection>>(attempt);
    auto* stream = connection->stream();
    if (stream == nullptr) {
        return PortStatus::error(failure, "候选传输不提供字节流面", false,
                                 PortFailureCategory::Transport);
    }
    // 别名共享指针:字节面持有连接本身,防止连接被提前释放。
    stream_out =
        std::shared_ptr<net_client::ISecureByteStream>(connection, stream);
    connection_out = std::move(connection);
    return PortStatus::success();
}

PortValue<std::unique_ptr<GatewaySession>> WireLoginTransport::connect_gateway(
    std::span<const net_client::EndpointCandidate> candidates,
    TimePoint deadline) {
    PortValue<std::unique_ptr<GatewaySession>> result;
    std::shared_ptr<net_client::ISecureConnection> connection;
    std::shared_ptr<net_client::ISecureByteStream> stream;
    result.status = connect_racing(Segment::Gateway, candidates, connection,
                                   stream, deadline);
    if (!result.status.ok) {
        return result;
    }
    result.value = std::make_unique<WireGatewaySession>(
        std::move(connection), std::move(stream));
    return result;
}

PortStatus WireLoginTransport::attach(GatewaySession& session,
                                      std::string_view identity_token,
                                      std::string_view queue_number_token,
                                      TimePoint deadline) {
    auto* wire_session = dynamic_cast<WireGatewaySession*>(&session);
    if (wire_session == nullptr) {
        return PortStatus::error(ChainFailure::GatewayConnectFailed,
                                 "attach 收到不匹配的 Gateway Session");
    }
    auto& edge = wire_session->edge();
    common::EdgeAttach message;
    message.set_identity_token(std::string{identity_token});
    message.set_queue_number_token(std::string{queue_number_token});
    if (!edge.send_frame(common::encode(message, 0), deadline)) {
        return PortStatus::error(ChainFailure::AttachRejected,
                                 "attach 帧发送失败", false,
                                 PortFailureCategory::Transport);
    }
    for (;;) {
        const auto payload = edge.receive_frame(deadline);
        if (!payload.has_value()) {
            return PortStatus::error(ChainFailure::AttachRejected,
                                     "未在窗口内收到受理", false,
                                     PortFailureCategory::Timeout);
        }
        const auto message_id = common::edge_message_id(*payload);
        if (!message_id.has_value()) {
            return PortStatus::error(ChainFailure::AttachRejected, "坏帧");
        }
        if (*message_id == edge_v1::MESSAGE_ID_S2C_EDGE_ATTACH_ACCEPTED) {
            return PortStatus::success();
        }
        if (*message_id == edge_v1::MESSAGE_ID_S2C_ERROR) {
            const auto error = common::decode_edge_error(*payload);
            // 2001:号牌无效/过期 → 交由上层自动重取(spec §7)。
            const bool expired =
                error.has_value() &&
                static_cast<int>(error->code()) ==
                    common::edge_error_invalid_queue_number;
            return PortStatus::error(
                ChainFailure::AttachRejected,
                error.has_value() ? edge_error_detail(*error) : "attach 被拒",
                expired);
        }
        // 其余帧(不应出现):忽略继续等受理。
    }
}

PortValue<HandoffResult> WireLoginTransport::await_handoff(
    GatewaySession& session,
    TimePoint deadline) {
    PortValue<HandoffResult> result;
    auto* wire_session = dynamic_cast<WireGatewaySession*>(&session);
    if (wire_session == nullptr) {
        result.status = PortStatus::error(ChainFailure::GatewayConnectFailed,
                                          "handoff 收到不匹配的 Gateway Session");
        return result;
    }
    auto& edge = wire_session->edge();
    for (;;) {
        const auto payload = edge.receive_frame(deadline);
        if (!payload.has_value()) {
            result.status = PortStatus::error(ChainFailure::HandoffTimeout,
                                              "未在窗口内收到交付", false,
                                              PortFailureCategory::Timeout);
            return result;
        }
        const auto message_id = common::edge_message_id(*payload);
        if (!message_id.has_value()) {
            result.status =
                PortStatus::error(ChainFailure::HandoffRejected, "坏帧");
            return result;
        }
        if (*message_id == edge_v1::MESSAGE_ID_S2C_ENTER_REALM_GRANTED) {
            const auto granted = common::decode_enter_realm_granted(*payload);
            if (!granted.has_value() || granted->enter_realm_ticket().empty()) {
                result.status =
                    PortStatus::error(ChainFailure::HandoffRejected,
                                      "交付缺票据");
                return result;
            }
            result.value.enter_realm_ticket = granted->enter_realm_ticket();
            result.value.realm_endpoints.reserve(
                static_cast<std::size_t>(granted->realm_endpoints_size()));
            for (const auto& endpoint : granted->realm_endpoints()) {
                result.value.realm_endpoints.push_back(
                    to_endpoint_candidate(endpoint));
            }
            result.status = PortStatus::success();
            return result;
        }
        if (*message_id == edge_v1::MESSAGE_ID_S2C_ERROR) {
            const auto error = common::decode_edge_error(*payload);
            result.status = PortStatus::error(
                ChainFailure::HandoffRejected,
                error.has_value() ? edge_error_detail(*error) : "交付被拒");
            return result;
        }
    }
}

PortValue<std::unique_ptr<RealmSession>> WireLoginTransport::connect_realm(
    std::span<const net_client::EndpointCandidate> candidates,
    TimePoint deadline) {
    PortValue<std::unique_ptr<RealmSession>> result;
    std::shared_ptr<net_client::ISecureConnection> connection;
    std::shared_ptr<net_client::ISecureByteStream> stream;
    result.status = connect_racing(Segment::Realm, candidates, connection,
                                   stream, deadline);
    if (!result.status.ok) {
        return result;
    }
    result.value = std::make_unique<WireRealmSession>(
        std::move(connection), std::move(stream));
    return result;
}

PortStatus WireLoginTransport::enter_realm(RealmSession& session,
                                           std::string_view enter_realm_ticket,
                                           TimePoint deadline) {
    auto* wire_session = dynamic_cast<WireRealmSession*>(&session);
    if (wire_session == nullptr) {
        return PortStatus::error(ChainFailure::RealmConnectFailed,
                                 "兑换收到不匹配的 Realm Session");
    }
    return redeemer_.redeem(wire_session->stream(), enter_realm_ticket,
                            deadline);
}

}  // namespace realm::client
