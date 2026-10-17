#include "realmmesh/edge/edge_runtime.hpp"

#include "realmmesh/network/transport/transport_factory.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace realm::edge {

EdgeRuntime::EdgeRuntime(
    const std::vector<network::TransportConfig>& transports,
    EdgeRuntimeOptions options,
    EdgeSourceConfig source_config,
    observability::Logger* logger)
    : EdgeRuntime(
          network::TransportFactory::create_enabled(transports, logger),
          options,
          std::move(source_config)) {}

EdgeRuntime::EdgeRuntime(
    std::vector<std::unique_ptr<network::IMessageTransport>> transports,
    EdgeRuntimeOptions options)
    : EdgeRuntime(
          std::move(transports), options, EdgeSourceConfig{}) {}

EdgeRuntime::EdgeRuntime(
    std::vector<std::unique_ptr<network::IMessageTransport>> transports,
    EdgeRuntimeOptions options,
    EdgeSourceConfig source_config)
    : transports_(std::move(transports)),
      options_(options),
      source_normalizer_(std::move(source_config)),
      inbound_(options_.inbound_capacity),
      outbound_(options_.outbound_capacity) {
    if (transports_.empty()) {
        throw std::invalid_argument(
            "edge runtime must have at least one enabled transport");
    }
    for (const auto& transport : transports_) {
        connections_.register_transport(*transport);
    }
    local_port_ = transports_.front()->local_endpoint().port;
    local_endpoints_.reserve(transports_.size());
    for (const auto& transport : transports_) {
        local_endpoints_.push_back(transport->local_endpoint());
    }
    if (options_.max_commands_per_cycle == 0) {
        throw std::invalid_argument(
            "max commands per I/O cycle must be positive");
    }
    if (options_.io_poll_interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("I/O poll interval must be positive");
    }
}

EdgeRuntime::~EdgeRuntime() { stop(); }

void EdgeRuntime::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    try {
        io_thread_ = std::jthread([this](std::stop_token stop_token) {
            io_loop(stop_token);
        });
    } catch (...) {
        running_.store(false);
        throw;
    }
}

void EdgeRuntime::stop() noexcept {
    running_.store(false);
    if (io_thread_.joinable()) {
        io_thread_.request_stop();
        io_thread_.join();
    }
}

bool EdgeRuntime::running() const noexcept { return running_.load(); }

std::uint16_t EdgeRuntime::local_port() const noexcept {
    return local_port_;
}

std::optional<network::TransportEndpoint> EdgeRuntime::local_endpoint(
    std::string_view transport_name) const {
    const auto iterator = std::ranges::find_if(
        local_endpoints_, [transport_name](const auto& endpoint) {
            return endpoint.name == transport_name;
        });
    if (iterator == local_endpoints_.end()) {
        return std::nullopt;
    }
    return *iterator;
}

const std::vector<network::TransportEndpoint>& EdgeRuntime::local_endpoints()
    const noexcept {
    return local_endpoints_;
}

std::optional<EdgeEvent> EdgeRuntime::try_receive() {
    return inbound_.try_pop();
}

std::vector<EdgeEvent> EdgeRuntime::drain_events(std::size_t max_events) {
    return inbound_.drain(max_events);
}

QueueResult EdgeRuntime::try_send(
    EdgeConnectionId connection_id, std::span<const std::byte> payload) {
    return enqueue({
        .kind = CommandKind::Send,
        .connection_id = connection_id,
        .payload = std::vector<std::byte>(payload.begin(), payload.end()),
    });
}

QueueResult EdgeRuntime::try_accept(
    EdgeConnectionId connection_id, std::span<const std::byte> response) {
    return enqueue({
        .kind = CommandKind::Accept,
        .connection_id = connection_id,
        .payload = std::vector<std::byte>(response.begin(), response.end()),
    });
}

QueueResult EdgeRuntime::try_decline(
    EdgeConnectionId connection_id, std::span<const std::byte> response) {
    return enqueue({
        .kind = CommandKind::Decline,
        .connection_id = connection_id,
        .payload = std::vector<std::byte>(response.begin(), response.end()),
    });
}

QueueResult EdgeRuntime::try_close(EdgeConnectionId connection_id) {
    return enqueue({
        .kind = CommandKind::Close,
        .connection_id = connection_id,
        .payload = {},
    });
}

QueueResult EdgeRuntime::try_reload_credentials() {
    return enqueue({
        .kind = CommandKind::ReloadCredentials,
        .connection_id = invalid_edge_connection_id,
        .payload = {},
    });
}

EdgeRuntimeStats EdgeRuntime::stats() const noexcept {
    return {
        .overload_disconnects = overload_disconnects_.load(),
        .rejected_outbound_commands = rejected_outbound_commands_.load(),
        .unknown_session_commands = unknown_session_commands_.load(),
        .successful_deliveries = successful_deliveries_.load(),
        .failed_deliveries = failed_deliveries_.load(),
        .invalid_source_disconnects = invalid_source_disconnects_.load(),
    };
}

std::optional<std::string> EdgeRuntime::terminal_error() const {
    std::scoped_lock lock(terminal_error_mutex_);
    return terminal_error_;
}

QueueResult EdgeRuntime::enqueue(OutboundCommand command) {
    if (!running()) {
        return QueueResult::Stopped;
    }
    if (!outbound_.try_push(std::move(command))) {
        rejected_outbound_commands_.fetch_add(1);
        return QueueResult::Full;
    }
    return QueueResult::Queued;
}

void EdgeRuntime::io_loop(std::stop_token stop_token) noexcept {
    try {
        while (!stop_token.stop_requested()) {
            process_outbound_commands();
            for (auto event : poll_events(options_.io_poll_interval)) {
                publish_event(std::move(event));
            }
        }
        process_outbound_commands();
    } catch (const std::exception& error) {
        {
            std::scoped_lock lock(terminal_error_mutex_);
            terminal_error_ = error.what();
        }
        failed_deliveries_.fetch_add(1);
    } catch (...) {
        {
            std::scoped_lock lock(terminal_error_mutex_);
            terminal_error_ = "unknown exception in edge runtime I/O loop";
        }
        failed_deliveries_.fetch_add(1);
    }
    running_.store(false);
}

void EdgeRuntime::process_outbound_commands() {
    for (auto& command : outbound_.drain(options_.max_commands_per_cycle)) {
        process_command(std::move(command));
    }
}

void EdgeRuntime::process_command(OutboundCommand command) {
    switch (command.kind) {
    case CommandKind::Send: {
        // established-only 契约:pending 或未知连接的发送都是契约违例。
        const auto record = connections_.record(command.connection_id);
        if (!record.has_value() || !record->established) {
            unknown_session_commands_.fetch_add(1);
            break;
        }
        if (connections_.send(command.connection_id, command.payload) ==
            SendResult::Sent) {
            successful_deliveries_.fetch_add(1);
        } else {
            failed_deliveries_.fetch_add(1);
            // Queued 只代表 Runtime 已接管命令；实际交付失败必须让上层
            // 最终观察到 ConnectionClosed，避免管线永久保留会话与额度。
            static_cast<void>(finish_close(command.connection_id));
        }
        break;
    }
    case CommandKind::Accept: {
        const auto record = connections_.record(command.connection_id);
        if (!record.has_value() || record->established) {
            unknown_session_commands_.fetch_add(1);
            break;
        }
        if (connections_.send(command.connection_id, command.payload) !=
            SendResult::Sent) {
            failed_deliveries_.fetch_add(1);
            // 响应发不出去,连接不可用:终结并通知业务层。
            static_cast<void>(finish_close(command.connection_id));
            break;
        }
        static_cast<void>(connections_.establish(command.connection_id));
        successful_deliveries_.fetch_add(1);
        publish_event({
            .kind = EdgeEventKind::ConnectionEstablished,
            .connection_id = command.connection_id,
            .protocol = record->primary.protocol,
            .established = true,
            .payload = {},
        });
        break;
    }
    case CommandKind::Decline: {
        const auto record = connections_.record(command.connection_id);
        if (!record.has_value()) {
            unknown_session_commands_.fetch_add(1);
            break;
        }
        if (!command.payload.empty()) {
            if (connections_.send(command.connection_id, command.payload) ==
                SendResult::Sent) {
                successful_deliveries_.fetch_add(1);
            } else {
                failed_deliveries_.fetch_add(1);
            }
        }
        static_cast<void>(finish_close(command.connection_id));
        break;
    }
    case CommandKind::Close:
        if (!finish_close(command.connection_id)) {
            unknown_session_commands_.fetch_add(1);
        }
        break;
    case CommandKind::ReloadCredentials:
        if (!std::ranges::all_of(transports_, [](const auto& transport) {
                return transport->reload_credentials();
            })) {
            failed_deliveries_.fetch_add(1);
        }
        break;
    }
}

bool EdgeRuntime::finish_close(EdgeConnectionId connection_id) {
    const auto record = connections_.record(connection_id);
    if (!record.has_value()) {
        return false;
    }
    const auto closed = connections_.close(connection_id);
    if (!closed.has_value()) {
        return false;
    }
    publish_event({
        .kind = EdgeEventKind::ConnectionClosed,
        .connection_id = closed->connection_id,
        .protocol = record->primary.protocol,
        .established = closed->established,
        .payload = {},
        .source = closed->source,
    });
    return true;
}

std::vector<EdgeEvent> EdgeRuntime::poll_events(
    std::chrono::milliseconds timeout) {
    const auto count =
        static_cast<std::chrono::milliseconds::rep>(transports_.size());
    const auto per_transport_timeout =
        count == 0 ? std::chrono::milliseconds::zero() : timeout / count;
    std::vector<EdgeEvent> edge_events;

    for (const auto& transport : transports_) {
        const auto events = transport->poll_once(per_transport_timeout);
        for (auto& event : events) {
            switch (event.kind) {
            case network::TransportEventKind::SessionOpened: {
                const auto source = source_normalizer_.normalize(event.source);
                if (!source.has_value()) {
                    invalid_source_disconnects_.fetch_add(1U);
                    static_cast<void>(transport->close(event.session_id));
                    break;
                }
                edge_events.push_back({
                    .kind = EdgeEventKind::ConnectionOpened,
                    .connection_id = connections_.open(
                        transport->name(), event.session_id, *source),
                    .protocol = transport->protocol(),
                    .established = false,
                    .payload = {},
                    .source = *source,
                });
                break;
            }
            case network::TransportEventKind::SessionClosed: {
                // 已被本地关闭的连接此处查不到记录:终结已由 finish_close
                // 的合成事件上报,不重复发布。
                const auto closed = connections_.on_transport_closed(
                    transport->name(), event.session_id);
                if (closed.has_value()) {
                    edge_events.push_back({
                        .kind = EdgeEventKind::ConnectionClosed,
                        .connection_id = closed->connection_id,
                        .protocol = transport->protocol(),
                        .established = closed->established,
                        .payload = {},
                        .source = closed->source,
                    });
                }
                break;
            }
            case network::TransportEventKind::MessageReceived:
            case network::TransportEventKind::PeerAddressChanged: {
                const auto connection_id =
                    connections_.find(transport->name(), event.session_id);
                if (!connection_id.has_value()) {
                    break;  // 连接已终结,丢弃迟到帧
                }
                const auto record = connections_.record(*connection_id);
                if (!record.has_value()) {
                    break;
                }
                auto source = record->source;
                if (event.kind ==
                    network::TransportEventKind::PeerAddressChanged) {
                    const auto normalized =
                        source_normalizer_.normalize(event.source);
                    if (!normalized.has_value()) {
                        invalid_source_disconnects_.fetch_add(1U);
                        static_cast<void>(finish_close(*connection_id));
                        break;
                    }
                    source = *normalized;
                    static_cast<void>(
                        connections_.update_source(*connection_id, source));
                }
                edge_events.push_back({
                    .kind =
                        event.kind ==
                                network::TransportEventKind::MessageReceived
                            ? EdgeEventKind::MessageReceived
                            : EdgeEventKind::PeerAddressChanged,
                    .connection_id = *connection_id,
                    .protocol = transport->protocol(),
                    .established = record->established,
                    .payload = std::move(event.payload),
                    .source = std::move(source),
                });
                break;
            }
            }
        }
    }
    return edge_events;
}

void EdgeRuntime::publish_event(EdgeEvent event) {
    const auto kind = event.kind;
    const auto connection_id = event.connection_id;
    if (inbound_.try_push(std::move(event))) {
        return;
    }

    if (kind != EdgeEventKind::ConnectionClosed) {
        // 队列满即视为该连接不可再服务;由此产生的终结事件同样进不了
        // 队列,业务层以超时断连兜底。
        overload_disconnects_.fetch_add(1);
        static_cast<void>(connections_.close(connection_id));
    }
}

}  // namespace realm::edge
