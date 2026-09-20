#include "realmmesh/game/queue/queue_store.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realm::game::queue {
namespace {

using Json = nlohmann::json;

constexpr std::string_view gateway_prefix = "/realmmesh/budgets/service/gateway/";
constexpr std::string_view realm_prefix = "/realmmesh/budgets/service/realm/";
constexpr std::string_view snapshot_key = "/realmmesh/queue/snapshot";

std::string base64_encode(std::string_view input) {
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
        const std::uint32_t value = (static_cast<std::uint32_t>(first) << 16U) |
                                    (static_cast<std::uint32_t>(second) << 8U) |
                                    static_cast<std::uint32_t>(third);
        output.push_back(alphabet[(value >> 18U) & 0x3FU]);
        output.push_back(alphabet[(value >> 12U) & 0x3FU]);
        output.push_back(
            offset + 1U < input.size() ? alphabet[(value >> 6U) & 0x3FU] : '=');
        output.push_back(
            offset + 2U < input.size() ? alphabet[value & 0x3FU] : '=');
    }
    return output;
}

/// 按调用顺序吐出预置应答的 etcd 客户端;nullopt 应答 = 调用失败。
class ScriptedEtcdClient final : public cluster::IEtcdHttpClient {
public:
    void enqueue(std::optional<std::string> response) {
        scripted_.push_back(std::move(response));
    }

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>&
    calls() const {
        return calls_;
    }

    std::optional<std::string> post(
        std::string_view path,
        std::string_view json_body,
        std::string* error) override {
        calls_.emplace_back(std::string(path), std::string(json_body));
        if (scripted_.empty()) {
            if (error != nullptr) *error = "no scripted response";
            return std::nullopt;
        }
        auto response = std::move(scripted_.front());
        scripted_.pop_front();
        if (!response.has_value()) {
            if (error != nullptr) *error = "etcd unavailable";
            return std::nullopt;
        }
        return response;
    }

private:
    std::deque<std::optional<std::string>> scripted_;
    std::vector<std::pair<std::string, std::string>> calls_;
};

std::string range_response(
    std::initializer_list<std::string_view> values) {
    Json kvs = Json::array();
    for (const auto value : values) {
        kvs.push_back({
            {"key", base64_encode("/x/y/budget")},
            {"value", base64_encode(value)},
        });
    }
    return Json{{"kvs", std::move(kvs)}}.dump();
}

EtcdQueueStore::Options test_options() {
    return EtcdQueueStore::Options{
        .budget_prefix = "/realmmesh/budgets/service",
        .snapshot_key = std::string{snapshot_key},
    };
}

QueueSnapshot snapshot_with_release_batch() {
    return QueueSnapshot{
        .released_number = 5,
        .next_number = 9,
        .admit_rate = 100,
        .release_batches_pruned_through = 2,
        .release_batches = {
            {.first_number = 3,
             .last_number = 5,
             .released_at = std::chrono::system_clock::time_point{
                 std::chrono::seconds{1'700'000'000}}}},
    };
}

TEST(QueueStoreTest, RefreshBudgetsAggregatesBothPrefixes) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(range_response({R"({"conn_free":100,"fetch_free":30})",
                                    R"({"conn_free":"0","fetch_free":100})"}));
    client->enqueue(range_response({R"({"conn_free":40})"}));
    const EtcdQueueStore store(test_options(), client);

    const auto aggregate = store.refresh_budgets();
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ(aggregate->gateway_admission, 30U);  // 实例级 min:30 + 0
    EXPECT_EQ(aggregate->realm_connections, 40U);

    ASSERT_EQ(client->calls().size(), 2U);
    EXPECT_EQ(client->calls()[0].first, "/v3/kv/range");
    EXPECT_NE(client->calls()[0].second.find(
                  base64_encode(gateway_prefix)),
              std::string::npos);
    EXPECT_NE(client->calls()[1].second.find(
                  base64_encode(realm_prefix)),
              std::string::npos);
    // 额度按前缀查询(§5.2 一前缀聚合全部实例):请求必须携带 range_end
    // 上界;缺了就退化成精确 key 读取,永远查不到任何实例。
    EXPECT_NE(client->calls()[0].second.find("range_end"), std::string::npos);
    EXPECT_NE(client->calls()[1].second.find("range_end"), std::string::npos);
}

TEST(QueueStoreTest, RefreshBudgetsSkipsMalformedEntries) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(range_response({R"({"conn_free":10,"fetch_free":10})",
                                    R"({"conn_free":-5,"fetch_free":10})",
                                    "not-json"}));
    client->enqueue(range_response({R"({"conn_free":7})",
                                    R"({"fetch_free":99})"}));
    const EtcdQueueStore store(test_options(), client);
    const auto aggregate = store.refresh_budgets();
    ASSERT_TRUE(aggregate.has_value());
    EXPECT_EQ(aggregate->gateway_admission, 10U);
    EXPECT_EQ(aggregate->realm_connections, 7U);
}

TEST(QueueStoreTest, RefreshBudgetsFailsClosedOnEtcdError) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(range_response({R"({"conn_free":100,"fetch_free":100})"}));
    client->enqueue(std::nullopt);  // realm 前缀调用失败
    const EtcdQueueStore store(test_options(), client);
    EXPECT_FALSE(store.refresh_budgets().has_value());
}

TEST(QueueStoreTest, SaveSnapshotPutsEncodedValue) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json{{"header", {}}}.dump());
    const EtcdQueueStore store(test_options(), client);

    EXPECT_TRUE(store.save_snapshot(
        snapshot_with_release_batch(), 1'700'000'001));

    ASSERT_EQ(client->calls().size(), 1U);
    EXPECT_EQ(client->calls()[0].first, "/v3/kv/put");
    EXPECT_NE(
        client->calls()[0].second.find(base64_encode(snapshot_key)),
        std::string::npos);
    EXPECT_NE(
        client->calls()[0].second.find(
            base64_encode(
                R"({"admit_rate":100,"next_number":9,"release_batches":[{"first_number":3,"last_number":5,"released_at":1700000000}],"release_batches_pruned_through":2,"released_number":5,"updated_at":1700000001})")),
        std::string::npos);
}

TEST(QueueStoreTest, SaveSnapshotReturnsFalseOnFailure) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(std::nullopt);
    const EtcdQueueStore store(test_options(), client);
    EXPECT_FALSE(store.save_snapshot(snapshot_with_release_batch(), 0));
}

TEST(QueueStoreTest, LoadSnapshotParsesStoredValue) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(range_response(
        {R"({"released_number":5,"next_number":9,"admit_rate":100,"release_batches_pruned_through":2,"release_batches":[{"first_number":3,"last_number":5,"released_at":1700000000}]})"}));
    const EtcdQueueStore store(test_options(), client);
    const auto snapshot = store.load_snapshot();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(*snapshot, snapshot_with_release_batch());
    // 精确 key 读取:请求体带 key、不带 range_end。
    ASSERT_EQ(client->calls().size(), 1U);
    EXPECT_NE(
        client->calls()[0].second.find(base64_encode(snapshot_key)),
        std::string::npos);
    EXPECT_EQ(
        client->calls()[0].second.find("range_end"), std::string::npos);
}

TEST(QueueStoreTest, LegacySnapshotRestoresAsExpiredPrefix) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(range_response(
        {R"({"released_number":5,"next_number":9,"admit_rate":100})"}));
    const EtcdQueueStore store(test_options(), client);
    const auto snapshot = store.load_snapshot();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->release_batches_pruned_through, 5U);
    EXPECT_TRUE(snapshot->release_batches.empty());
}

TEST(QueueStoreTest, LoadSnapshotReturnsNulloptWhenMissing) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json{{"count", 0}}.dump());
    const EtcdQueueStore store(test_options(), client);
    EXPECT_FALSE(store.load_snapshot().has_value());
}

// 冷备三态契约:不可达/损坏抛出(启动失败,fail-closed),仅缺失从零。
TEST(QueueStoreTest, LoadSnapshotFailsClosedOnUnavailableOrCorrupt) {
    const EtcdQueueStore store(test_options(), [] {
        auto client = std::make_shared<ScriptedEtcdClient>();
        client->enqueue(std::nullopt);  // etcd 不可用
        return client;
    }());
    EXPECT_THROW(store.load_snapshot(), std::runtime_error);

    auto inconsistent = std::make_shared<ScriptedEtcdClient>();
    inconsistent->enqueue(range_response(
        {R"({"released_number":9,"next_number":5,"admit_rate":0})"}));
    const EtcdQueueStore inconsistent_store(test_options(), inconsistent);
    EXPECT_THROW(inconsistent_store.load_snapshot(), std::runtime_error);

    auto malformed = std::make_shared<ScriptedEtcdClient>();
    malformed->enqueue(range_response({"not-json"}));
    const EtcdQueueStore malformed_store(test_options(), malformed);
    EXPECT_THROW(malformed_store.load_snapshot(), std::runtime_error);

    auto missing_field = std::make_shared<ScriptedEtcdClient>();
    missing_field->enqueue(range_response(
        {R"({"released_number":5,"next_number":9})"}));
    const EtcdQueueStore missing_field_store(test_options(), missing_field);
    EXPECT_THROW(missing_field_store.load_snapshot(), std::runtime_error);

    auto gap = std::make_shared<ScriptedEtcdClient>();
    gap->enqueue(range_response(
        {R"({"released_number":5,"next_number":9,"admit_rate":0,"release_batches_pruned_through":1,"release_batches":[{"first_number":3,"last_number":5,"released_at":100}]})"}));
    const EtcdQueueStore gap_store(test_options(), gap);
    EXPECT_THROW(gap_store.load_snapshot(), std::runtime_error);

    auto malformed_batch = std::make_shared<ScriptedEtcdClient>();
    malformed_batch->enqueue(range_response(
        {R"({"released_number":5,"next_number":9,"admit_rate":0,"release_batches_pruned_through":2,"release_batches":[{"first_number":3,"last_number":5,"released_at":100,"extra":true}]})"}));
    const EtcdQueueStore malformed_batch_store(
        test_options(), malformed_batch);
    EXPECT_THROW(
        malformed_batch_store.load_snapshot(), std::runtime_error);
}

}  // namespace
}  // namespace realm::game::queue
