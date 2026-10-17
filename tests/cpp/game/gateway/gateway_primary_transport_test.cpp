#include "realmmesh/game/gateway/gateway_primary_transport.hpp"

#include "realmmesh/edge/edge_runtime.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace realm::game::gateway {
namespace {

class DeliveryFailingTransport final : public network::IMessageTransport {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "memory";
    }
    [[nodiscard]] network::TransportProtocol protocol() const noexcept override {
        return network::TransportProtocol::TlsTcp;
    }
    [[nodiscard]] network::TransportEndpoint local_endpoint() const override {
        return {"memory", protocol(), "127.0.0.1", 0};
    }
    [[nodiscard]] std::size_t session_count() const noexcept override {
        return 1;
    }
    [[nodiscard]] std::vector<network::TransportEvent> poll_once(
        std::chrono::milliseconds timeout) override {
        polling_.store(true);
        std::this_thread::sleep_for(timeout);
        polling_.store(false);
        if (opened_.exchange(true)) return {};
        return {{
            .kind = network::TransportEventKind::SessionOpened,
            .session_id = 1,
            .payload = {},
            .source = {.direct_peer = "127.0.0.1"},
        }};
    }
    [[nodiscard]] bool send(
        network::SessionId,
        std::span<const std::byte>) override {
        return !fail_delivery_.load();
    }
    [[nodiscard]] bool close(network::SessionId) override { return true; }
    [[nodiscard]] bool reload_credentials() override { return true; }

    void fail_delivery() noexcept { fail_delivery_.store(true); }
    [[nodiscard]] bool polling() const noexcept { return polling_.load(); }

private:
    std::atomic_bool opened_{false};
    std::atomic_bool fail_delivery_{false};
    std::atomic_bool polling_{false};
};

[[nodiscard]] std::optional<edge::EdgeEvent> wait_for_event(
    GatewayPrimaryTransport& transport, edge::EdgeEventKind kind) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& event : transport.drain_events(8)) {
            if (event.kind == kind) return event;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

TEST(InMemoryGatewayPrimaryTransportTest, DrainsEventsWithABound) {
    InMemoryGatewayPrimaryTransport transport;
    transport.push_event(
        {.kind = edge::EdgeEventKind::ConnectionOpened,
         .connection_id = edge::EdgeConnectionId{1}});
    transport.push_event(
        {.kind = edge::EdgeEventKind::MessageReceived,
         .connection_id = edge::EdgeConnectionId{1}});
    transport.push_event(
        {.kind = edge::EdgeEventKind::ConnectionClosed,
         .connection_id = edge::EdgeConnectionId{1}});

    const auto first = transport.drain_events(2);
    ASSERT_EQ(first.size(), 2U);
    EXPECT_EQ(first[0].kind, edge::EdgeEventKind::ConnectionOpened);
    EXPECT_EQ(first[1].kind, edge::EdgeEventKind::MessageReceived);

    const auto second = transport.drain_events(2);
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].kind, edge::EdgeEventKind::ConnectionClosed);
}

TEST(InMemoryGatewayPrimaryTransportTest, ScriptsFullAndStoppedIndependently) {
    InMemoryGatewayPrimaryTransport transport;
    transport.script_result(
        PrimaryTransportCommandKind::Accept,
        {PrimaryTransportResult::Full, PrimaryTransportResult::Stopped});
    const std::array payload{std::byte{1}};

    EXPECT_EQ(
        transport.accept(edge::EdgeConnectionId{7}, payload),
        PrimaryTransportResult::Full);
    EXPECT_EQ(
        transport.accept(edge::EdgeConnectionId{7}, payload),
        PrimaryTransportResult::Stopped);
    EXPECT_EQ(
        transport.close(edge::EdgeConnectionId{7}), PrimaryTransportResult::Stopped);
    EXPECT_TRUE(transport.owned_commands().empty());

    InMemoryGatewayPrimaryTransport running_transport;
    EXPECT_EQ(
        running_transport.close(edge::EdgeConnectionId{7}),
        PrimaryTransportResult::Queued);
    ASSERT_EQ(running_transport.owned_commands().size(), 1U);
    EXPECT_EQ(
        running_transport.owned_commands().front().kind,
        PrimaryTransportCommandKind::Close);
}

TEST(InMemoryGatewayPrimaryTransportTest, RecordsOnlyQueuedSemanticCommands) {
    InMemoryGatewayPrimaryTransport transport;
    const std::array payload{std::byte{2}, std::byte{3}};

    EXPECT_EQ(
        transport.accept(edge::EdgeConnectionId{1}, payload),
        PrimaryTransportResult::Queued);
    EXPECT_EQ(
        transport.decline(edge::EdgeConnectionId{2}, payload),
        PrimaryTransportResult::Queued);
    EXPECT_EQ(
        transport.send_handoff(edge::EdgeConnectionId{3}, payload),
        PrimaryTransportResult::Queued);
    EXPECT_EQ(
        transport.close(edge::EdgeConnectionId{4}), PrimaryTransportResult::Queued);

    ASSERT_EQ(transport.owned_commands().size(), 4U);
    EXPECT_EQ(
        transport.owned_commands()[0].kind,
        PrimaryTransportCommandKind::Accept);
    EXPECT_EQ(
        transport.owned_commands()[1].kind,
        PrimaryTransportCommandKind::Decline);
    EXPECT_EQ(
        transport.owned_commands()[2].kind,
        PrimaryTransportCommandKind::SendHandoff);
    EXPECT_EQ(
        transport.owned_commands()[3].kind,
        PrimaryTransportCommandKind::Close);
    EXPECT_EQ(transport.owned_commands()[2].payload.size(), 2U);
}

TEST(EdgeRuntimePrimaryTransportTest, MapsStoppedWithoutTakingOwnership) {
    auto lower_transport = std::make_unique<DeliveryFailingTransport>();
    std::vector<std::unique_ptr<network::IMessageTransport>> transports;
    transports.push_back(std::move(lower_transport));
    edge::EdgeRuntime runtime(
        std::move(transports),
        {.inbound_capacity = 8, .outbound_capacity = 8});
    EdgeRuntimePrimaryTransport transport(runtime);

    EXPECT_EQ(
        transport.close(edge::EdgeConnectionId{1}),
        PrimaryTransportResult::Stopped);
}

TEST(EdgeRuntimePrimaryTransportTest, MapsRuntimeQueueBackpressureToFull) {
    auto lower_transport = std::make_unique<DeliveryFailingTransport>();
    auto* lower_transport_view = lower_transport.get();
    std::vector<std::unique_ptr<network::IMessageTransport>> transports;
    transports.push_back(std::move(lower_transport));
    edge::EdgeRuntime runtime(
        std::move(transports),
        {.inbound_capacity = 8,
         .outbound_capacity = 1,
         .io_poll_interval = std::chrono::milliseconds(100)});
    EdgeRuntimePrimaryTransport transport(runtime);
    runtime.start();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!lower_transport_view->polling() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(lower_transport_view->polling());

    EXPECT_EQ(
        transport.close(edge::EdgeConnectionId{10}),
        PrimaryTransportResult::Queued);
    EXPECT_EQ(
        transport.close(edge::EdgeConnectionId{11}), PrimaryTransportResult::Full);
    runtime.stop();
}

TEST(
    EdgeRuntimePrimaryTransportTest,
    QueuedDeliveryFailureEventuallyPublishesSessionClosed) {
    auto lower_transport = std::make_unique<DeliveryFailingTransport>();
    auto* lower_transport_view = lower_transport.get();
    std::vector<std::unique_ptr<network::IMessageTransport>> transports;
    transports.push_back(std::move(lower_transport));
    edge::EdgeRuntime runtime(
        std::move(transports),
        {.inbound_capacity = 8,
         .outbound_capacity = 8,
         .io_poll_interval = std::chrono::milliseconds(1)});
    EdgeRuntimePrimaryTransport transport(runtime);
    runtime.start();

    const auto opened =
        wait_for_event(transport, edge::EdgeEventKind::ConnectionOpened);
    ASSERT_TRUE(opened.has_value());
    const std::array payload{std::byte{1}};
    ASSERT_EQ(
        transport.accept(opened->connection_id, payload),
        PrimaryTransportResult::Queued);
    ASSERT_TRUE(
        wait_for_event(transport, edge::EdgeEventKind::ConnectionEstablished)
            .has_value());

    lower_transport_view->fail_delivery();
    ASSERT_EQ(
        transport.send_handoff(opened->connection_id, payload),
        PrimaryTransportResult::Queued);
    const auto closed =
        wait_for_event(transport, edge::EdgeEventKind::ConnectionClosed);
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->connection_id, opened->connection_id);
    EXPECT_EQ(runtime.stats().failed_deliveries, 1U);
    runtime.stop();
}

TEST(
    EdgeRuntimePrimaryTransportTest,
    DeclineTerminatesAnEstablishedSessionForPipelineProtocolRejection) {
    auto lower_transport = std::make_unique<DeliveryFailingTransport>();
    std::vector<std::unique_ptr<network::IMessageTransport>> transports;
    transports.push_back(std::move(lower_transport));
    edge::EdgeRuntime runtime(
        std::move(transports),
        {.inbound_capacity = 8,
         .outbound_capacity = 8,
         .io_poll_interval = std::chrono::milliseconds(1)});
    EdgeRuntimePrimaryTransport transport(runtime);
    runtime.start();

    const auto opened =
        wait_for_event(transport, edge::EdgeEventKind::ConnectionOpened);
    ASSERT_TRUE(opened.has_value());
    const std::array payload{std::byte{1}};
    ASSERT_EQ(
        transport.accept(opened->connection_id, payload),
        PrimaryTransportResult::Queued);
    ASSERT_TRUE(wait_for_event(transport, edge::EdgeEventKind::ConnectionEstablished)
                    .has_value());

    ASSERT_EQ(
        transport.decline(opened->connection_id, payload),
        PrimaryTransportResult::Queued);
    const auto closed =
        wait_for_event(transport, edge::EdgeEventKind::ConnectionClosed);
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->connection_id, opened->connection_id);
    EXPECT_TRUE(closed->established);
    runtime.stop();
}

}  // namespace
}  // namespace realm::game::gateway
