#include "realmmesh/game/chat/chat_module.hpp"
#include "realmmesh/game/chat/chat_channels.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/network/transport/transport_factory.hpp"
#include "realmmesh/observability/metrics_registry.hpp"
#include "realmmesh/service_host/service_module.hpp"
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>

namespace realm::game::chat {
namespace {
class ChatModule final : public service_host::ServiceModule {
public:
    explicit ChatModule(const service_host::ServiceModuleContext& context)
        : config_(load_chat_config(context.config)), instance_(context.instance_id),
          metrics_(context.metrics), logger_(context.logger) {
        for (auto transport : context.config.edge().transports) {
            if (!transport.enabled) continue;
            if (transport.protocol != network::TransportProtocol::TlsTcp || !transport.tls ||
                transport.tls->alpn != common::chat_internal_alpn || transport.tls->client_ca_file.empty() ||
                transport.tls->allowed_client_identities != std::vector<std::string>{config_.realm_identity} ||
                transport.max_sessions == 0 || transport.max_sessions > common::chat_transport_connections_max ||
                transport.max_payload_size == 0 || transport.max_payload_size > common::chat_envelope_max ||
                transport.max_pending_output_bytes == 0 || transport.max_pending_output_bytes > common::chat_link_outbound_bytes_max)
                throw std::invalid_argument("Chat requires one bounded TLS listener with its exact configured Realm identity");
            // 限制每连接每轮解密工作；SSL缓冲在下一轮继续消费。
            transport.max_receive_batch_bytes = common::chat_envelope_max;
            transports_.push_back(std::move(transport));
        }
        if (transports_.size() != 1) throw std::invalid_argument("Chat requires exactly one TLS listener");
    }
    void start() override {
        if (transport_) return;
        channels_ = std::make_unique<ChatChannels>(config_, instance_,
            common::correlation_id_hex(common::make_correlation_id()));
        auto transports = network::TransportFactory::create_enabled(transports_, &logger_);
        transport_ = std::move(transports.front());
        endpoints_ = {transport_->local_endpoint()};
        metrics_.counter_set("chat_ingress_rejected_total", 0);
        for (const auto outcome : {"accepted", "rejected", "unknown"})
            metrics_.counter_set("chat_send_total", 0, {{"outcome", outcome}});
        publish();
    }
    void stop() override {
        transport_.reset(); channels_.reset(); endpoints_.clear(); waiting_hello_.clear();
    }
    bool running() const noexcept override { return static_cast<bool>(transport_); }
    bool ready() const noexcept override { return running(); }
    std::span<const network::TransportEndpoint> local_endpoints() const noexcept override { return endpoints_; }
    void tick(const service_host::ServiceTickContext&) override {
        if (!transport_) return;
        const auto now = ChatChannels::Clock::now();
        std::set<ChatChannels::Peer> closed;
        const auto close = [&](ChatChannels::Peer peer) {
            static_cast<void>(transport_->close(peer)); channels_->closed(peer);
            waiting_hello_.erase(peer); closed.insert(peer);
        };
        std::size_t received = 0;
        for (const auto& event : transport_->poll_once(std::chrono::milliseconds(0))) {
            const auto peer = event.session_id;
            if (closed.contains(peer)) continue;
            if (event.kind == network::TransportEventKind::SessionOpened) {
                if (!channels_->opened(peer)) close(peer);
                else waiting_hello_[peer] = now + std::chrono::milliseconds(config_.operation_timeout_ms);
            } else if (event.kind == network::TransportEventKind::SessionClosed) close(peer);
            else if (event.kind == network::TransportEventKind::MessageReceived) {
                if (received++ >= common::chat_requests_per_round_max) {
                    metrics_.counter_add("chat_ingress_rejected_total"); close(peer); continue;
                }
                const auto request = common::decode_chat_internal_request(event.payload);
                const auto id = common::chat_internal_request_id(event.payload);
                if (!request || !id || (waiting_hello_.contains(peer) && request->operation() != wire::OPERATION_HELLO)) {
                    close(peer); continue;
                }
                if (!channels_->submit(peer, *request, *id, now)) {
                    const auto busy = chat_reply(*request, wire::STATUS_BUSY);
                    observe(busy, 0);
                    if (!transport_->send(peer, common::encode(busy, *id))) close(peer);
                }
            }
        }
        channels_->run_round(ChatChannels::Clock::now());
        for (const auto peer : channels_->take_closed_peers()) close(peer);
        while (const auto reply = channels_->take_reply()) {
            if (closed.contains(reply->peer)) continue;
            observe(reply->message, reply->duration_seconds);
            if (!transport_->send(reply->peer, common::encode(reply->message, reply->request_id))) {
                close(reply->peer); continue;
            }
            if (reply->message.operation() == wire::OPERATION_HELLO) {
                if (reply->message.status() == wire::STATUS_OK) waiting_hello_.erase(reply->peer);
                else close(reply->peer);
            }
        }
        for (const auto& push : channels_->drain_pushes()) {
            if (!closed.contains(push.peer) && !transport_->send(push.peer, push.payload)) close(push.peer);
        }
        std::vector<ChatChannels::Peer> expired;
        for (const auto& [peer, deadline] : waiting_hello_) if (ChatChannels::Clock::now() >= deadline) expired.push_back(peer);
        for (const auto peer : expired) close(peer);
        publish();
    }
private:
    void observe(const common::ChatInternalReply& reply, double seconds) {
        const auto operation = wire::Operation_Name(reply.operation());
        const auto status = wire::Status_Name(reply.status());
        metrics_.counter_add("chat_requests_total", 1, {{"operation", operation}, {"status", status}});
        metrics_.histogram_observe("chat_operation_duration_seconds", seconds, {{"operation", operation}});
        if (reply.operation() == wire::OPERATION_SEND) {
            const std::string_view outcome = reply.has_result() && reply.result().outcome() == wire::OUTCOME_ACCEPTED ? "accepted" : "rejected";
            metrics_.counter_add("chat_send_total", 1, {{"outcome", outcome}});
            metrics_.histogram_observe("chat_send_duration_seconds", seconds, {{"outcome", outcome}});
        }
    }
    void publish() {
        const auto& s = channels_->stats();
        metrics_.counter_set("chat_messages_accepted_total", static_cast<double>(s.accepted));
        metrics_.counter_set("chat_receipt_replays_total", static_cast<double>(s.replayed));
        metrics_.counter_set("chat_queue_rejected_total", static_cast<double>(s.queue_rejected));
        metrics_.counter_set("chat_deadline_expired_total", static_cast<double>(s.expired));
        metrics_.counter_set("chat_rate_rejected_total", static_cast<double>(s.rate_rejected));
        metrics_.counter_set("chat_stale_rejected_total", static_cast<double>(s.stale_rejected));
        metrics_.counter_set("chat_resource_rejected_total", static_cast<double>(s.resource_rejected));
        metrics_.gauge_set("chat_members", static_cast<double>(channels_->active_members()));
        metrics_.gauge_set("chat_pending_requests", static_cast<double>(channels_->pending_requests()));
        metrics_.gauge_set("chat_pending_pushes", static_cast<double>(channels_->pending_pushes()));
        metrics_.gauge_set("chat_pending_push_bytes", static_cast<double>(channels_->pending_push_bytes()));
    }
    ChatConfig config_;
    std::string instance_;
    observability::MetricsRegistry& metrics_;
    observability::Logger& logger_;
    std::vector<network::TransportConfig> transports_;
    std::unique_ptr<network::IMessageTransport> transport_;
    std::unique_ptr<ChatChannels> channels_;
    std::vector<network::TransportEndpoint> endpoints_;
    std::map<ChatChannels::Peer, ChatChannels::Clock::time_point> waiting_hello_;
};
}  // namespace
std::unique_ptr<service_host::ServiceModule> make_chat_module(const service_host::ServiceModuleContext& context) {
    return std::make_unique<ChatModule>(context);
}
}  // namespace realm::game::chat
