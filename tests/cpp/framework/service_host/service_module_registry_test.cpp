#include "realmmesh/service_host/service_module.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace realm::service_host {
namespace {

ServiceModuleRegistration null_factory_registration(
    std::optional<std::string> dependency = std::nullopt) {
    return {
        .factory = [](const ServiceModuleContext&) {
            return std::unique_ptr<ServiceModule>{};
        },
        .dependency = std::move(dependency),
    };
}

TEST(ServiceModuleRegistryTest, FindsRegisteredModuleByName) {
    ServiceModuleRegistry registry;
    registry.add("queue", null_factory_registration());
    registry.add(
        "gateway",
        {
            .factory = null_factory_registration().factory,
            .dependency = "realm",
            .mode2_waits_for_dependency = true,
        });

    const auto* queue = registry.find("queue");
    ASSERT_NE(queue, nullptr);
    EXPECT_FALSE(queue->dependency.has_value());
    EXPECT_FALSE(queue->mode2_waits_for_dependency);

    const auto* gateway = registry.find("gateway");
    ASSERT_NE(gateway, nullptr);
    EXPECT_EQ(gateway->dependency, std::optional<std::string>{"realm"});
    EXPECT_TRUE(gateway->mode2_waits_for_dependency);
}

TEST(ServiceModuleRegistryTest, UnregisteredNameReturnsNull) {
    ServiceModuleRegistry registry;
    registry.add("queue", null_factory_registration());
    EXPECT_EQ(registry.find("login_verify"), nullptr);
    EXPECT_EQ(registry.find(""), nullptr);
}

/// 服务名即集群服务类型:非法字符在注册期拒绝，而不是等到发现注册。
TEST(ServiceModuleRegistryTest, RejectsInvalidServiceName) {
    ServiceModuleRegistry registry;
    EXPECT_THROW(
        registry.add("", null_factory_registration()), std::invalid_argument);
    EXPECT_THROW(
        registry.add("Login-Verify", null_factory_registration()),
        std::invalid_argument);
    EXPECT_THROW(
        registry.add("gateway", null_factory_registration("realm/1")),
        std::invalid_argument);
    EXPECT_EQ(registry.find("gateway"), nullptr);
}

TEST(ServiceModuleRegistryTest, RejectsDuplicateNameAndMissingFactory) {
    ServiceModuleRegistry registry;
    registry.add("queue", null_factory_registration());
    EXPECT_THROW(
        registry.add("queue", null_factory_registration()),
        std::invalid_argument);
    EXPECT_THROW(
        registry.add("realm", ServiceModuleRegistration{}),
        std::invalid_argument);
    EXPECT_EQ(registry.find("realm"), nullptr);
}

}  // namespace
}  // namespace realm::service_host
