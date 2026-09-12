#include "realmmesh/cluster/budget_publisher.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "realmmesh/test_support/fake_service_registry.hpp"

namespace realm::cluster {
namespace {

using Json = nlohmann::json;
using test_support::FakeServiceRegistry;
using SysClock = std::chrono::system_clock;

const SysClock::time_point t0{std::chrono::seconds{1726100000}};

InstanceBudgetSnapshot gateway_snapshot(
    std::uint64_t conn_free, std::uint64_t fetch_free) {
    return {.conn_free = conn_free,
            .fetch_free = fetch_free,
            .has_fetch = true};
}

ServiceInstance minimal_instance(ServiceType type, std::string instance_id) {
    return {
        .type = type,
        .instance_id = std::move(instance_id),
        .node_id = "node-01",
        .zone = "development",
        .endpoints =
            {
                {
                    .name = "client",
                    .protocol = network::TransportProtocol::TlsTcp,
                    .address = "127.0.0.1",
                    .port = 8000,
                },
            },
        .weight = 100,
        .version = "0.1.0",
    };
}

/// put_leased 按预设结果序列返回失败的注册中心(其余方法不参与)。
class FlakyRegistry final : public IServiceRegistry {
public:
    explicit FlakyRegistry(std::deque<bool> outcomes)
        : outcomes_(std::move(outcomes)) {}

    RegistrationResult register_instance(
        const ServiceInstance&, std::chrono::seconds) override {
        return {RegistryStatus::Success, 1};
    }
    bool refresh_registration(RegistrationId) override { return true; }
    bool unregister_instance(RegistrationId) override { return true; }
    std::vector<ServiceInstance> discover(ServiceType) const override {
        return {};
    }
    WatchId watch(ServiceType, ServiceEventHandler) override { return 1; }
    bool cancel_watch(WatchId) override { return true; }

    bool put_leased(
        RegistrationId, std::string_view key, std::string_view value) override {
        if (outcomes_.empty()) {
            ADD_FAILURE() << "unexpected put_leased: " << key;
            return false;
        }
        const bool ok = outcomes_.front();
        outcomes_.pop_front();
        if (ok) {
            puts_.emplace_back(std::string(key), std::string(value));
        }
        return ok;
    }

    std::vector<std::pair<std::string, std::string>> puts_;

private:
    std::deque<bool> outcomes_;
};

TEST(BudgetPublishPolicyTest, FirstPublishAlwaysPublishes) {
    const BudgetPublishPolicy policy;
    EXPECT_TRUE(policy.should_publish(
        std::nullopt, InstanceBudgetSnapshot{}, t0, t0));
    EXPECT_TRUE(policy.should_publish(
        std::nullopt, gateway_snapshot(0, 0), t0, t0));
}

TEST(BudgetPublishPolicyTest, UnchangedSnapshotNeverRepublishes) {
    const BudgetPublishPolicy policy;
    const auto snapshot = gateway_snapshot(98, 10);
    EXPECT_FALSE(policy.should_publish(snapshot, snapshot, t0, t0 + std::chrono::hours(1)));
}

TEST(BudgetPublishPolicyTest, ChangeBeyondThresholdPublishesImmediately) {
    const BudgetPublishPolicy policy;
    EXPECT_TRUE(policy.should_publish(
        gateway_snapshot(100, 10),
        gateway_snapshot(89, 10),
        t0,
        t0 + std::chrono::milliseconds(100)));
    EXPECT_TRUE(policy.should_publish(
        gateway_snapshot(100, 10),
        gateway_snapshot(110, 10),
        t0,
        t0 + std::chrono::milliseconds(100)));
}

TEST(BudgetPublishPolicyTest, ChangeWithinThresholdWaitsForInterval) {
    const BudgetPublishPolicy policy;
    EXPECT_FALSE(policy.should_publish(
        gateway_snapshot(100, 10),
        gateway_snapshot(95, 10),
        t0,
        t0 + std::chrono::milliseconds(999)));
    EXPECT_TRUE(policy.should_publish(
        gateway_snapshot(100, 10),
        gateway_snapshot(95, 10),
        t0,
        t0 + std::chrono::milliseconds(1000)));
}

TEST(BudgetPublishPolicyTest, DropToZeroAndGrowthFromZeroAreBeyondThreshold) {
    const BudgetPublishPolicy policy;
    EXPECT_TRUE(policy.should_publish(
        gateway_snapshot(100, 10),
        gateway_snapshot(0, 10),
        t0,
        t0));
    EXPECT_TRUE(policy.should_publish(
        gateway_snapshot(0, 10),
        gateway_snapshot(1, 10),
        t0,
        t0));
}

TEST(BudgetPublishPolicyTest, FetchChangeTriggersOnItsOwn) {
    const BudgetPublishPolicy policy;
    EXPECT_TRUE(policy.should_publish(
        gateway_snapshot(100, 10),
        gateway_snapshot(100, 8),
        t0,
        t0));
    EXPECT_TRUE(policy.should_publish(
        gateway_snapshot(100, 0),
        gateway_snapshot(100, 1),
        t0,
        t0));
}

TEST(BudgetPublishPolicyTest, FetchPresenceMismatchCountsAsChange) {
    const BudgetPublishPolicy policy;
    EXPECT_TRUE(policy.should_publish(
        InstanceBudgetSnapshot{.conn_free = 100, .has_fetch = false},
        gateway_snapshot(100, 5),
        t0,
        t0));
}

TEST(BudgetPublishPolicyTest, ReturnToFullPublishesImmediately) {
    BudgetPublishPolicy policy;
    policy.conn_capacity = 100;
    policy.fetch_capacity = 10;
    // 回满(free 回到容量)即发布:放行阀门需要立即知道实例可全额接纳。
    EXPECT_TRUE(policy.should_publish(
        gateway_snapshot(95, 9),
        gateway_snapshot(100, 10),
        t0,
        t0 + std::chrono::milliseconds(100)));
    EXPECT_TRUE(policy.should_publish(
        gateway_snapshot(95, 9),
        gateway_snapshot(95, 10),
        t0,
        t0 + std::chrono::milliseconds(100)));
    // 未回满且变化低于阈值:仍按间隔节流。
    EXPECT_FALSE(policy.should_publish(
        gateway_snapshot(95, 9),
        gateway_snapshot(99, 9),
        t0,
        t0 + std::chrono::milliseconds(100)));
    // 容量未知(默认 0)时不触发回满语义。
    const BudgetPublishPolicy default_policy;
    EXPECT_FALSE(default_policy.should_publish(
        gateway_snapshot(95, 9),
        gateway_snapshot(100, 9),
        t0,
        t0 + std::chrono::milliseconds(100)));
}

TEST(BudgetKeyTest, BuildsBudgetKeyFromTypeAndInstance) {
    EXPECT_EQ(
        budget_key(ServiceType::Gateway, "gateway-01"),
        "/realmmesh/budgets/service/gateway/gateway-01/budget");
    EXPECT_EQ(
        budget_key(ServiceType::Realm, "realm-01"),
        "/realmmesh/budgets/service/realm/realm-01/budget");
}

TEST(InstanceBudgetReporterTest, PublishesFirstSnapshotUnderRegistrationLease) {
    FakeServiceRegistry registry;
    const auto registration = registry.register_instance(
        minimal_instance(ServiceType::Gateway, "gateway-01"),
        std::chrono::seconds(9));
    ASSERT_EQ(registration.status, RegistryStatus::Success);

    InstanceBudgetReporter reporter(
        registry, registration.id, ServiceType::Gateway, "gateway-01");
    EXPECT_TRUE(reporter.publish(gateway_snapshot(98, 10), t0));

    const auto keys = registry.leased_keys(registration.id);
    ASSERT_EQ(keys.size(), 1U);
    const auto found = keys.find("/realmmesh/budgets/service/gateway/gateway-01/budget");
    ASSERT_NE(found, keys.end());
    EXPECT_EQ(
        found->second,
        R"({"conn_free":98,"fetch_free":10,"updated_at":1726100000})");
}

TEST(InstanceBudgetReporterTest, SkipsUnchangedSnapshot) {
    FakeServiceRegistry registry;
    const auto registration = registry.register_instance(
        minimal_instance(ServiceType::Gateway, "gateway-01"),
        std::chrono::seconds(9));
    ASSERT_EQ(registration.status, RegistryStatus::Success);

    InstanceBudgetReporter reporter(
        registry, registration.id, ServiceType::Gateway, "gateway-01");
    ASSERT_TRUE(reporter.publish(gateway_snapshot(98, 10), t0));
    EXPECT_FALSE(
        reporter.publish(gateway_snapshot(98, 10), t0 + std::chrono::hours(1)));

    EXPECT_EQ(registry.leased_keys(registration.id).size(), 1U);
}

TEST(InstanceBudgetReporterTest, HoldsSubThresholdChangeWithinInterval) {
    FakeServiceRegistry registry;
    const auto registration = registry.register_instance(
        minimal_instance(ServiceType::Gateway, "gateway-01"),
        std::chrono::seconds(9));
    ASSERT_EQ(registration.status, RegistryStatus::Success);

    InstanceBudgetReporter reporter(
        registry, registration.id, ServiceType::Gateway, "gateway-01");
    ASSERT_TRUE(reporter.publish(gateway_snapshot(100, 10), t0));
    EXPECT_FALSE(reporter.publish(
        gateway_snapshot(95, 10), t0 + std::chrono::milliseconds(500)));
    EXPECT_TRUE(reporter.publish(
        gateway_snapshot(95, 10), t0 + std::chrono::milliseconds(1000)));

    const auto value =
        registry.leased_keys(registration.id)
            .at("/realmmesh/budgets/service/gateway/gateway-01/budget");
    EXPECT_EQ(value, R"({"conn_free":95,"fetch_free":10,"updated_at":1726100001})");
}

TEST(InstanceBudgetReporterTest, RepublishesBeyondThresholdImmediately) {
    FakeServiceRegistry registry;
    const auto registration = registry.register_instance(
        minimal_instance(ServiceType::Gateway, "gateway-01"),
        std::chrono::seconds(9));
    ASSERT_EQ(registration.status, RegistryStatus::Success);

    InstanceBudgetReporter reporter(
        registry, registration.id, ServiceType::Gateway, "gateway-01");
    ASSERT_TRUE(reporter.publish(gateway_snapshot(100, 10), t0));
    EXPECT_TRUE(reporter.publish(
        gateway_snapshot(50, 10), t0 + std::chrono::milliseconds(1)));
}

TEST(InstanceBudgetReporterTest, RetriesFailedPublishAfterInterval) {
    FlakyRegistry registry({false, true});
    InstanceBudgetReporter reporter(
        registry, 1, ServiceType::Gateway, "gateway-01");

    EXPECT_FALSE(reporter.publish(gateway_snapshot(98, 10), t0));
    EXPECT_FALSE(
        reporter.publish(gateway_snapshot(98, 10), t0 + std::chrono::milliseconds(500)));
    ASSERT_TRUE(
        reporter.publish(gateway_snapshot(98, 10), t0 + std::chrono::milliseconds(1000)));

    ASSERT_EQ(registry.puts_.size(), 1U);
    EXPECT_EQ(
        registry.puts_.front().first,
        "/realmmesh/budgets/service/gateway/gateway-01/budget");
}

TEST(InstanceBudgetReporterTest, RealmReporterOmitsFetchFree) {
    FakeServiceRegistry registry;
    const auto registration = registry.register_instance(
        minimal_instance(ServiceType::Realm, "realm-01"), std::chrono::seconds(9));
    ASSERT_EQ(registration.status, RegistryStatus::Success);

    InstanceBudgetReporter reporter(
        registry, registration.id, ServiceType::Realm, "realm-01");
    EXPECT_TRUE(reporter.publish(
        InstanceBudgetSnapshot{.conn_free = 50, .has_fetch = false}, t0));

    const auto value = registry.leased_keys(registration.id)
                           .at("/realmmesh/budgets/service/realm/realm-01/budget");
    EXPECT_EQ(value, R"({"conn_free":50,"updated_at":1726100000})");
}

TEST(InstanceBudgetReporterTest, UnknownRegistrationFailsWithoutRetry) {
    FakeServiceRegistry registry;
    InstanceBudgetReporter reporter(
        registry, 999, ServiceType::Gateway, "gateway-01");

    EXPECT_FALSE(reporter.publish(gateway_snapshot(98, 10), t0));
    EXPECT_FALSE(
        reporter.publish(gateway_snapshot(98, 10), t0 + std::chrono::hours(1)));
}

TEST(InstanceBudgetReporterTest, FailedWriteNotifiesSinkNotThrottledRetries) {
    FlakyRegistry registry({false, true, false});
    InstanceBudgetReporter reporter(
        registry, 1, ServiceType::Gateway, "gateway-01");
    std::vector<std::string> failures;
    reporter.set_failure_sink(
        [&failures](const std::string& key) { failures.push_back(key); });

    EXPECT_FALSE(reporter.publish(gateway_snapshot(98, 10), t0));
    // 节流中的重试未触达存储,不重复记录。
    EXPECT_FALSE(
        reporter.publish(gateway_snapshot(98, 10), t0 + std::chrono::milliseconds(500)));
    ASSERT_TRUE(
        reporter.publish(gateway_snapshot(98, 10), t0 + std::chrono::milliseconds(1000)));
    // 恢复后再失败,重新记录(每次真实写失败恰好一条)。
    EXPECT_FALSE(
        reporter.publish(gateway_snapshot(50, 10), t0 + std::chrono::milliseconds(1001)));

    ASSERT_EQ(failures.size(), 2U);
    EXPECT_EQ(
        failures.front(),
        "/realmmesh/budgets/service/gateway/gateway-01/budget");
    EXPECT_EQ(
        failures.back(),
        "/realmmesh/budgets/service/gateway/gateway-01/budget");
}

}  // namespace
}  // namespace realm::cluster
