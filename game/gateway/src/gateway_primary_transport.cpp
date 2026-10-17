#include "realmmesh/game/gateway/gateway_primary_transport.hpp"

#include "realmmesh/edge/edge_runtime.hpp"

#include <algorithm>
#include <utility>

namespace realm::game::gateway {
namespace {

[[nodiscard]] PrimaryTransportResult translate(edge::QueueResult result) {
    switch (result) {
    case edge::QueueResult::Queued:
        return PrimaryTransportResult::Queued;
    case edge::QueueResult::Full:
        return PrimaryTransportResult::Full;
    case edge::QueueResult::Stopped:
        return PrimaryTransportResult::Stopped;
    }
    return PrimaryTransportResult::Stopped;
}

[[nodiscard]] std::size_t index(PrimaryTransportCommandKind kind) {
    return static_cast<std::size_t>(kind);
}

}  // namespace

std::vector<edge::EdgeEvent> EdgeRuntimePrimaryTransport::drain_events(
    std::size_t max_events) {
    return runtime_->drain_events(max_events);
}

PrimaryTransportResult EdgeRuntimePrimaryTransport::accept(
    edge::EdgeConnectionId session_id, std::span<const std::byte> response) {
    return translate(runtime_->try_accept(session_id, response));
}

PrimaryTransportResult EdgeRuntimePrimaryTransport::decline(
    edge::EdgeConnectionId session_id, std::span<const std::byte> response) {
    return translate(runtime_->try_decline(session_id, response));
}

PrimaryTransportResult EdgeRuntimePrimaryTransport::send_handoff(
    edge::EdgeConnectionId session_id, std::span<const std::byte> message) {
    return translate(runtime_->try_send(session_id, message));
}

PrimaryTransportResult EdgeRuntimePrimaryTransport::close(
    edge::EdgeConnectionId session_id) {
    return translate(runtime_->try_close(session_id));
}

void InMemoryGatewayPrimaryTransport::push_event(edge::EdgeEvent event) {
    events_.push_back(std::move(event));
}

void InMemoryGatewayPrimaryTransport::script_result(
    PrimaryTransportCommandKind kind,
    std::deque<PrimaryTransportResult> results) {
    results_[index(kind)] = std::move(results);
}

std::vector<edge::EdgeEvent> InMemoryGatewayPrimaryTransport::drain_events(
    std::size_t max_events) {
    const auto count = std::min(max_events, events_.size());
    std::vector<edge::EdgeEvent> drained;
    drained.reserve(count);
    for (std::size_t position = 0; position < count; ++position) {
        drained.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    return drained;
}

PrimaryTransportResult InMemoryGatewayPrimaryTransport::accept(
    edge::EdgeConnectionId session_id, std::span<const std::byte> response) {
    return execute(PrimaryTransportCommandKind::Accept, session_id, response);
}

PrimaryTransportResult InMemoryGatewayPrimaryTransport::decline(
    edge::EdgeConnectionId session_id, std::span<const std::byte> response) {
    return execute(PrimaryTransportCommandKind::Decline, session_id, response);
}

PrimaryTransportResult InMemoryGatewayPrimaryTransport::send_handoff(
    edge::EdgeConnectionId session_id, std::span<const std::byte> message) {
    return execute(
        PrimaryTransportCommandKind::SendHandoff, session_id, message);
}

PrimaryTransportResult InMemoryGatewayPrimaryTransport::close(
    edge::EdgeConnectionId session_id) {
    return execute(PrimaryTransportCommandKind::Close, session_id, {});
}

PrimaryTransportResult InMemoryGatewayPrimaryTransport::execute(
    PrimaryTransportCommandKind kind,
    edge::EdgeConnectionId session_id,
    std::span<const std::byte> payload) {
    if (stopped_) return PrimaryTransportResult::Stopped;
    auto& script = results_[index(kind)];
    const auto result = script.empty() ? PrimaryTransportResult::Queued
                                       : script.front();
    if (!script.empty()) script.pop_front();
    if (result == PrimaryTransportResult::Stopped) {
        stopped_ = true;
        return result;
    }
    if (result == PrimaryTransportResult::Queued) {
        owned_commands_.push_back(
            {kind,
             session_id,
             std::vector<std::byte>(payload.begin(), payload.end())});
    }
    return result;
}

}  // namespace realm::game::gateway
