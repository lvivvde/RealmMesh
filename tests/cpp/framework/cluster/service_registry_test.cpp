#include "realmmesh/cluster/service_registry.hpp"
#include "realmmesh/cluster/service_publisher.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/test_support/fake_service_registry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace realm::cluster {
namespace {

ServiceInstance make_instance(
    ServiceType type,
    std::string instance_id,
    std::uint16_t port) {
    return {
        .type = type,
        .instance_id = std::move(instance_id),
        .node_id = "node-01",
        .zone = "development",
        .endpoints = {{
            .name = "rpc_tcp",
            .protocol = network::TransportProtocol::TlsTcp,
            .address = "127.0.0.1",
            .port = port,
        }},
        .weight = 100,
        .version = "0.1.0",
    };
}

TEST(ServiceRegistryTest, RegistersAndDiscoversByServiceType) {
    using namespace std::chrono_literals;

    test_support::FakeServiceRegistry registry;
    const auto gateway = make_instance(ServiceType::Gateway, "gateway-01", 8100);
    const auto scene = make_instance(ServiceType::Scene, "scene-01", 8400);

    const auto gateway_registration = registry.register_instance(gateway, 10s);
    const auto scene_registration = registry.register_instance(scene, 10s);

    ASSERT_EQ(gateway_registration.status, RegistryStatus::Success);
    ASSERT_EQ(scene_registration.status, RegistryStatus::Success);
    EXPECT_NE(gateway_registration.id, scene_registration.id);
    EXPECT_EQ(registry.discover(ServiceType::Gateway), std::vector{gateway});
    EXPECT_EQ(registry.discover(ServiceType::Scene), std::vector{scene});
    EXPECT_TRUE(registry.discover(ServiceType::Login).empty());
}

TEST(ServiceRegistryTest, RejectsDuplicateServiceInstanceKey) {
    using namespace std::chrono_literals;

    test_support::FakeServiceRegistry registry;
    const auto instance = make_instance(ServiceType::Gateway, "gateway-01", 8100);

    ASSERT_EQ(
        registry.register_instance(instance, 10s).status,
        RegistryStatus::Success);
    const auto duplicate = registry.register_instance(instance, 10s);

    EXPECT_EQ(duplicate.status, RegistryStatus::AlreadyExists);
    EXPECT_EQ(duplicate.id, invalid_registration_id);
}

TEST(ServiceRegistryTest, PublishesAddedAndRemovedWatchEvents) {
    using namespace std::chrono_literals;

    test_support::FakeServiceRegistry registry;
    std::vector<ServiceEvent> events;
    const auto watch = registry.watch(
        ServiceType::Gateway,
        [&events](const ServiceEvent& event) { events.push_back(event); });
    const auto instance = make_instance(ServiceType::Gateway, "gateway-01", 8100);

    const auto registration = registry.register_instance(instance, 10s);
    ASSERT_EQ(registration.status, RegistryStatus::Success);
    ASSERT_TRUE(registry.unregister_instance(registration.id));

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0], (ServiceEvent{ServiceEventKind::Added, instance}));
    EXPECT_EQ(events[1], (ServiceEvent{ServiceEventKind::Removed, instance}));
    EXPECT_TRUE(registry.cancel_watch(watch));
}

TEST(ServiceRegistryTest, ExpiredRegistrationDisappearsAndCannotRefresh) {
    using namespace std::chrono_literals;

    test_support::FakeServiceRegistry registry;
    const auto instance = make_instance(ServiceType::Scene, "scene-01", 8400);
    const auto registration = registry.register_instance(instance, 10s);
    ASSERT_EQ(registration.status, RegistryStatus::Success);

    EXPECT_TRUE(registry.refresh_registration(registration.id));
    EXPECT_TRUE(registry.expire_registration(registration.id));
    EXPECT_FALSE(registry.refresh_registration(registration.id));
    EXPECT_TRUE(registry.discover(ServiceType::Scene).empty());
}

TEST(ServiceRegistryTest, RejectsNonPositiveLeaseTtl) {
    test_support::FakeServiceRegistry registry;
    const auto instance = make_instance(ServiceType::Chat, "chat-01", 8600);

    const auto registration = registry.register_instance(
        instance,
        std::chrono::seconds::zero());

    EXPECT_EQ(registration.status, RegistryStatus::InvalidArgument);
    EXPECT_EQ(registration.id, invalid_registration_id);
}

TEST(ServiceRegistryTest, ResolverTracksAddedAndRemovedEndpoints) {
    using namespace std::chrono_literals;

    test_support::FakeServiceRegistry registry;
    ServiceResolver resolver(
        registry,
        ServiceType::Gateway,
        network::TransportProtocol::TlsTcp);
    const auto instance = make_instance(
        ServiceType::Gateway, "gateway-01", 8100);
    const auto registration = registry.register_instance(instance, 10s);
    ASSERT_EQ(registration.status, RegistryStatus::Success);

    const auto endpoint = resolver.endpoint();
    ASSERT_TRUE(endpoint.has_value());
    EXPECT_EQ(endpoint->address, "127.0.0.1");
    EXPECT_EQ(endpoint->port, 8100);

    ASSERT_TRUE(registry.unregister_instance(registration.id));
    EXPECT_FALSE(resolver.endpoint().has_value());
}

TEST(ServiceRegistryTest, ResolverReceivesInstancesPresentBeforeSubscription) {
    using namespace std::chrono_literals;

    test_support::FakeServiceRegistry registry;
    const auto instance = make_instance(
        ServiceType::Gateway, "gateway-01", 8100);
    ASSERT_EQ(
        registry.register_instance(instance, 10s).status,
        RegistryStatus::Success);

    ServiceResolver resolver(
        registry,
        ServiceType::Gateway,
        network::TransportProtocol::TlsTcp);

    ASSERT_TRUE(resolver.endpoint().has_value());
    EXPECT_EQ(resolver.endpoint()->port, 8100);
}

TEST(ServiceRegistryTest, PublisherRegistersAndUnregistersWithItsLifetime) {
    using namespace std::chrono_literals;

    test_support::FakeServiceRegistry registry;
    const auto instance = make_instance(ServiceType::Login, "login-01", 7000);
    {
        ServicePublisher publisher(registry, instance, 10s);
        EXPECT_TRUE(publisher.tick());
        EXPECT_TRUE(publisher.registered());
        EXPECT_EQ(registry.discover(ServiceType::Login), std::vector{instance});
    }

    EXPECT_TRUE(registry.discover(ServiceType::Login).empty());
}

}  // namespace
}  // namespace realm::cluster
