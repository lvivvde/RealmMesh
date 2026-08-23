#include "realmmesh/game/gateway/gateway_runtime.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace realm::game::gateway {

GatewayRuntime::GatewayRuntime(GatewayConfig config)
    : GatewayRuntime(config, config.runtime) {}

GatewayRuntime::GatewayRuntime(
    GatewayConfig config, GatewayRuntimeOptions options)
    : server_(std::move(config)),
      options_(options),
      local_port_(server_.local_port()),
      local_endpoints_(server_.local_endpoints()),
      inbound_(options_.inbound_capacity),
      outbound_(options_.outbound_capacity) {
    if (options_.max_commands_per_cycle == 0) {
        throw std::invalid_argument(
            "max commands per I/O cycle must be positive");
    }
    if (options_.io_poll_interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("I/O poll interval must be positive");
    }
}

GatewayRuntime::~GatewayRuntime() { stop(); }

void GatewayRuntime::start() {
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

void GatewayRuntime::stop() noexcept {
    running_.store(false);
    if (io_thread_.joinable()) {
        io_thread_.request_stop();
        io_thread_.join();
    }
}

bool GatewayRuntime::running() const noexcept { return running_.load(); }

std::uint16_t GatewayRuntime::local_port() const noexcept {
    return local_port_;
}

std::optional<network::TransportEndpoint> GatewayRuntime::local_endpoint(
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

const std::vector<network::TransportEndpoint>& GatewayRuntime::local_endpoints()
    const noexcept {
    return local_endpoints_;
}

std::optional<GatewayEvent> GatewayRuntime::try_receive() {
    return inbound_.try_pop();
}

std::vector<GatewayEvent> GatewayRuntime::drain_events(std::size_t max_events) {
    return inbound_.drain(max_events);
}

QueueResult GatewayRuntime::try_send(
    ClientSessionId client_session_id, std::span<const std::byte> payload) {
    return enqueue({
        .kind = CommandKind::SendClient,
        .client_session_id = client_session_id,
        .transport_name = {},
        .transport_session_id = network::invalid_session_id,
        .payload = std::vector<std::byte>(payload.begin(), payload.end()),
    });
}

QueueResult GatewayRuntime::try_send_channel(
    std::string_view transport_name,
    network::SessionId transport_session_id,
    std::span<const std::byte> payload) {
    return enqueue({
        .kind = CommandKind::SendChannel,
        .client_session_id = invalid_client_session_id,
        .transport_name = std::string(transport_name),
        .transport_session_id = transport_session_id,
        .payload = std::vector<std::byte>(payload.begin(), payload.end()),
    });
}

QueueResult GatewayRuntime::try_accept_connection(
    std::string_view transport_name,
    network::SessionId transport_session_id,
    std::span<const std::byte> response) {
    return enqueue({
        .kind = CommandKind::AcceptConnection,
        .client_session_id = invalid_client_session_id,
        .transport_name = std::string(transport_name),
        .transport_session_id = transport_session_id,
        .payload = std::vector<std::byte>(response.begin(), response.end()),
    });
}

QueueResult GatewayRuntime::try_close_channel(
    std::string_view transport_name, network::SessionId transport_session_id) {
    return enqueue({
        .kind = CommandKind::CloseChannel,
        .client_session_id = invalid_client_session_id,
        .transport_name = std::string(transport_name),
        .transport_session_id = transport_session_id,
        .payload = {},
    });
}

QueueResult GatewayRuntime::try_reload_credentials() {
    return enqueue({
        .kind = CommandKind::ReloadCredentials,
        .client_session_id = invalid_client_session_id,
        .transport_name = {},
        .transport_session_id = network::invalid_session_id,
        .payload = {},
    });
}

GatewayRuntimeStats GatewayRuntime::stats() const noexcept {
    return {
        .overload_disconnects = overload_disconnects_.load(),
        .rejected_outbound_commands = rejected_outbound_commands_.load(),
        .successful_deliveries = successful_deliveries_.load(),
        .failed_deliveries = failed_deliveries_.load(),
    };
}

std::optional<std::string> GatewayRuntime::terminal_error() const {
    std::scoped_lock lock(terminal_error_mutex_);
    return terminal_error_;
}

QueueResult GatewayRuntime::enqueue(OutboundCommand command) {
    if (!running()) {
        return QueueResult::Stopped;
    }
    if (!outbound_.try_push(std::move(command))) {
        rejected_outbound_commands_.fetch_add(1);
        return QueueResult::Full;
    }
    return QueueResult::Queued;
}

void GatewayRuntime::io_loop(std::stop_token stop_token) noexcept {
    try {
        while (!stop_token.stop_requested()) {
            process_outbound_commands();
            for (auto event : server_.poll_events(options_.io_poll_interval)) {
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
            terminal_error_ = "unknown exception in gateway I/O loop";
        }
        failed_deliveries_.fetch_add(1);
    }
    running_.store(false);
}

void GatewayRuntime::process_outbound_commands() {
    for (auto& command : outbound_.drain(options_.max_commands_per_cycle)) {
        process_command(std::move(command));
    }
}

void GatewayRuntime::process_command(OutboundCommand command) {
    switch (command.kind) {
    case CommandKind::SendClient: {
        const auto result =
            server_.send(command.client_session_id, command.payload);
        if (result == SendResult::Sent) {
            successful_deliveries_.fetch_add(1);
        } else {
            failed_deliveries_.fetch_add(1);
        }
        break;
    }
    case CommandKind::SendChannel:
        if (!server_.send_channel(
                command.transport_name,
                command.transport_session_id,
                command.payload)) {
            failed_deliveries_.fetch_add(1);
        } else {
            successful_deliveries_.fetch_add(1);
        }
        break;
    case CommandKind::AcceptConnection: {
        if (!server_.send_channel(
                command.transport_name,
                command.transport_session_id,
                command.payload)) {
            failed_deliveries_.fetch_add(1);
            static_cast<void>(server_.close_channel(
                command.transport_name, command.transport_session_id));
            break;
        }
        const auto client_id = server_.promote_connection(
            command.transport_name, command.transport_session_id);
        const auto endpoint = server_.local_endpoint(command.transport_name);
        if (!client_id.has_value() || !endpoint.has_value()) {
            failed_deliveries_.fetch_add(1);
            static_cast<void>(server_.close_channel(
                command.transport_name, command.transport_session_id));
            break;
        }
        successful_deliveries_.fetch_add(1);
        publish_event({
            .kind = GatewayEventKind::ClientSessionOpened,
            .client_session_id = client_id,
            .transport_name = std::move(command.transport_name),
            .protocol = endpoint->protocol,
            .transport_session_id = command.transport_session_id,
            .payload = {},
        });
        break;
    }
    case CommandKind::CloseChannel:
        if (!server_.close_channel(
                command.transport_name, command.transport_session_id)) {
            failed_deliveries_.fetch_add(1);
        }
        break;
    case CommandKind::ReloadCredentials:
        if (!server_.reload_credentials()) {
            failed_deliveries_.fetch_add(1);
        }
        break;
    }
}

void GatewayRuntime::publish_event(GatewayEvent event) {
    const auto kind = event.kind;
    const auto transport_name = event.transport_name;
    const auto transport_session_id = event.transport_session_id;
    if (inbound_.try_push(std::move(event))) {
        return;
    }

    if (kind != GatewayEventKind::ConnectionClosed) {
        overload_disconnects_.fetch_add(1);
        static_cast<void>(
            server_.close_channel(transport_name, transport_session_id));
    }
}

}  // namespace realm::game::gateway
