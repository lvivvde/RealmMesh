#include "realmmesh/cluster/etcd_service_registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realm::cluster {
namespace {

using Json = nlohmann::json;

class ScriptedEtcdHttpClient final : public IEtcdHttpClient {
public:
    struct Step {
        std::string path;
        std::optional<std::string> response;
        std::string error;
    };

    explicit ScriptedEtcdHttpClient(std::vector<Step> steps)
        : steps_(steps.begin(), steps.end()) {}

    std::optional<std::string> post(
        std::string_view path,
        std::string_view json_body,
        std::string* error) override {
        const std::scoped_lock lock(mutex_);
        if (steps_.empty()) {
            ADD_FAILURE() << "unexpected etcd request: " << path;
            return std::nullopt;
        }
        auto step = std::move(steps_.front());
        steps_.pop_front();
        EXPECT_EQ(path, step.path);
        requests_.push_back(Json::parse(json_body));
        if (!step.response.has_value() && error != nullptr) *error = step.error;
        return step.response;
    }

    [[nodiscard]] std::vector<Json> requests() const {
        const std::scoped_lock lock(mutex_);
        return requests_;
    }

    [[nodiscard]] bool complete() const {
        const std::scoped_lock lock(mutex_);
        return steps_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::deque<Step> steps_;
    std::vector<Json> requests_;
};

std::string base64(std::string_view input) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t offset = 0; offset < input.size(); offset += 3U) {
        const auto first = static_cast<unsigned char>(input[offset]);
        const auto second = offset + 1U < input.size()
            ? static_cast<unsigned char>(input[offset + 1U])
            : 0U;
        const auto third = offset + 2U < input.size()
            ? static_cast<unsigned char>(input[offset + 2U])
            : 0U;
        const auto value =
            (static_cast<std::uint32_t>(first) << 16U) |
            (static_cast<std::uint32_t>(second) << 8U) |
            static_cast<std::uint32_t>(third);
        output.push_back(alphabet[(value >> 18U) & 0x3FU]);
        output.push_back(alphabet[(value >> 12U) & 0x3FU]);
        output.push_back(offset + 1U < input.size()
                             ? alphabet[(value >> 6U) & 0x3FU]
                             : '=');
        output.push_back(offset + 2U < input.size()
                             ? alphabet[value & 0x3FU]
                             : '=');
    }
    return output;
}

EtcdRegistryOptions test_options() {
    return {
        .endpoint = "http://unused",
        .key_prefix = "/realmmesh/services",
        .request_timeout = std::chrono::milliseconds(50),
        .watch_interval = std::chrono::milliseconds(50),
        .background_maintenance = false,
    };
}

ServiceInstance gateway_instance() {
    return {
        .type = ServiceType::Gateway,
        .instance_id = "gateway-01",
        .node_id = "node-01",
        .zone = "development",
        .endpoints = {
            {
                .name = "client_quic",
                .protocol = network::TransportProtocol::Quic,
                .address = "127.0.0.1",
                .port = 8000,
            },
            {
                .name = "client_tls_tcp",
                .protocol = network::TransportProtocol::TlsTcp,
                .address = "127.0.0.1",
                .port = 8000,
            },
        },
        .weight = 100,
        .version = "0.1.0",
    };
}

std::string encoded_gateway_value() {
    return base64(Json({
        {"type", "gateway"},
        {"instance_id", "gateway-01"},
        {"node_id", "node-01"},
        {"zone", "development"},
        {"endpoints", Json::array({
            {
                {"name", "client_quic"},
                {"protocol", "quic"},
                {"address", "127.0.0.1"},
                {"port", 8000},
            },
            {
                {"name", "client_tls_tcp"},
                {"protocol", "tls_tcp"},
                {"address", "127.0.0.1"},
                {"port", 8000},
            },
        })},
        {"weight", 100},
        {"version", "0.1.0"},
    }).dump());
}

TEST(EtcdServiceRegistryTest, RegistersRefreshesAndRevokesLeasedInstance) {
    using Step = ScriptedEtcdHttpClient::Step;
    auto http = std::make_shared<ScriptedEtcdHttpClient>(std::vector<Step>{
        {"/v3/lease/grant", R"({"ID":"11","TTL":"9"})", {}},
        {"/v3/kv/txn", R"({"succeeded":true})", {}},
        {"/v3/lease/grant", R"({"ID":"12","TTL":"9"})", {}},
        {"/v3/kv/put", R"({})", {}},
        {"/v3/lease/revoke", R"({})", {}},
        {"/v3/lease/revoke", R"({})", {}},
    });
    EtcdServiceRegistry registry(test_options(), http);

    const auto registration =
        registry.register_instance(gateway_instance(), std::chrono::seconds(9));
    ASSERT_EQ(registration.status, RegistryStatus::Success);
    ASSERT_NE(registration.id, invalid_registration_id);
    EXPECT_TRUE(registry.refresh_registration(registration.id));
    EXPECT_TRUE(registry.unregister_instance(registration.id));
    EXPECT_TRUE(http->complete());

    const auto requests = http->requests();
    ASSERT_EQ(requests.size(), 6U);
    EXPECT_EQ(requests[0].at("TTL"), "9");
    EXPECT_EQ(
        requests[1].at("success").at(0).at("requestPut").at("lease"),
        "11");
    EXPECT_EQ(requests[3].at("lease"), "12");
    EXPECT_EQ(requests[4].at("ID"), "11");
    EXPECT_EQ(requests[5].at("ID"), "12");
}

TEST(EtcdServiceRegistryTest, RejectsDuplicateInstanceAtomically) {
    using Step = ScriptedEtcdHttpClient::Step;
    auto http = std::make_shared<ScriptedEtcdHttpClient>(std::vector<Step>{
        {"/v3/lease/grant", R"({"ID":"21","TTL":"10"})", {}},
        {"/v3/kv/txn", R"({"succeeded":false})", {}},
        {"/v3/lease/revoke", R"({})", {}},
    });
    EtcdServiceRegistry registry(test_options(), http);

    const auto result =
        registry.register_instance(gateway_instance(), std::chrono::seconds(10));

    EXPECT_EQ(result.status, RegistryStatus::AlreadyExists);
    EXPECT_EQ(result.id, invalid_registration_id);
    EXPECT_TRUE(http->complete());
}

TEST(EtcdServiceRegistryTest, DiscoversAndDecodesMultiProtocolEndpoints) {
    using Step = ScriptedEtcdHttpClient::Step;
    const auto response = Json({
        {"kvs", Json::array({{{"value", encoded_gateway_value()}}})},
    }).dump();
    auto http = std::make_shared<ScriptedEtcdHttpClient>(std::vector<Step>{
        {"/v3/kv/range", response, {}},
    });
    EtcdServiceRegistry registry(test_options(), http);

    EXPECT_EQ(
        registry.discover(ServiceType::Gateway),
        std::vector<ServiceInstance>{gateway_instance()});
    EXPECT_TRUE(http->complete());
}

TEST(EtcdServiceRegistryTest, PublishesSnapshotChangesToWatchers) {
    using Step = ScriptedEtcdHttpClient::Step;
    const auto populated = Json({
        {"kvs", Json::array({{{"value", encoded_gateway_value()}}})},
    }).dump();
    auto http = std::make_shared<ScriptedEtcdHttpClient>(std::vector<Step>{
        {"/v3/kv/range", R"({"kvs":[]})", {}},
        {"/v3/kv/range", populated, {}},
        {"/v3/kv/range", R"({"kvs":[]})", {}},
    });
    EtcdServiceRegistry registry(test_options(), http);
    std::vector<ServiceEvent> events;
    const auto watch = registry.watch(
        ServiceType::Gateway,
        [&events](const ServiceEvent& event) { events.push_back(event); });

    registry.poll_once();
    registry.poll_once();

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0], (ServiceEvent{ServiceEventKind::Added, gateway_instance()}));
    EXPECT_EQ(events[1], (ServiceEvent{ServiceEventKind::Removed, gateway_instance()}));
    EXPECT_TRUE(registry.cancel_watch(watch));
    EXPECT_TRUE(http->complete());
}

}  // namespace
}  // namespace realm::cluster
