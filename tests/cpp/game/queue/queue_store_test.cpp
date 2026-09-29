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

TEST(QueueStoreTest, NewIssueCommitsMappingAndSnapshotInOneTransaction) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json{{"ID", "123"}}.dump());
    client->enqueue(Json{{"succeeded", true}}.dump());
    const EtcdQueueStore store(test_options(), client);

    const auto result = store.issue_or_recover(QueueIssueRequest{
        .identity_jti = "0123456789abcdef0123456789abcdef",
        .issued_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'000'001}},
        .identity_expires_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'001'801}},
        .snapshot = snapshot_with_release_batch(),
    });

    EXPECT_EQ(result.status, QueueIssueStatus::Issued);
    EXPECT_EQ(result.number, 9U);
    EXPECT_EQ(result.snapshot.next_number, 10U);
    ASSERT_EQ(client->calls().size(), 2U);
    EXPECT_EQ(client->calls()[0].first, "/v3/lease/grant");
    EXPECT_EQ(client->calls()[1].first, "/v3/kv/txn");
    const auto transaction = Json::parse(client->calls()[1].second);
    ASSERT_EQ(transaction.at("compare").size(), 1U);
    ASSERT_EQ(transaction.at("success").size(), 2U);
}

TEST(QueueStoreTest, ExistingIdentityRecoversOriginalNumberAndIssueTime) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json{{"ID", "123"}}.dump());
    const std::string issuance_key =
        "/realmmesh/queue/issuance/a55798dd7966ed4b45bd53202cf7a982";
    const std::string issuance_value =
        R"({"identity_expires_at":1700001801,"issued_at":1700000001,"number":9})";
    const std::string snapshot_value =
        R"({"admit_rate":100,"next_number":10,"release_batches":[{"first_number":3,"last_number":5,"released_at":1700000000}],"release_batches_pruned_through":2,"released_number":5,"updated_at":1700000001})";
    client->enqueue(Json{
        {"succeeded", false},
        {"responses",
         Json::array({
             Json{{"response_range",
                   {{"kvs",
                     Json::array({Json{{"key", base64_encode(issuance_key)},
                                       {"value", base64_encode(issuance_value)}}})}}}},
             Json{{"response_range",
                   {{"kvs",
                     Json::array({Json{{"key", base64_encode(snapshot_key)},
                                       {"value", base64_encode(snapshot_value)}}})}}}},
         })},
    }.dump());
    const EtcdQueueStore store(test_options(), client);

    const auto result = store.issue_or_recover(QueueIssueRequest{
        .identity_jti = "0123456789abcdef0123456789abcdef",
        .issued_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'000'101}},
        .identity_expires_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'001'801}},
        .snapshot = snapshot_with_release_batch(),
    });

    EXPECT_EQ(result.status, QueueIssueStatus::Recovered);
    EXPECT_EQ(result.number, 9U);
    EXPECT_EQ(
        result.issued_at,
        std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'000'001}});
    EXPECT_EQ(result.snapshot.next_number, 10U);
}

TEST(QueueStoreTest, AmbiguousCommitReadsBackDurableIssue) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json{{"ID", "123"}}.dump());
    client->enqueue(std::nullopt);  // txn may have committed before reply loss
    const std::string issuance_key =
        "/realmmesh/queue/issuance/a55798dd7966ed4b45bd53202cf7a982";
    const std::string issuance_value =
        R"({"identity_expires_at":1700001801,"issued_at":1700000001,"number":9})";
    const std::string snapshot_value =
        R"({"admit_rate":100,"next_number":10,"release_batches":[{"first_number":3,"last_number":5,"released_at":1700000000}],"release_batches_pruned_through":2,"released_number":5,"updated_at":1700000001})";
    client->enqueue(Json{
        {"succeeded", true},
        {"responses",
         Json::array({
             Json{{"response_range",
                   {{"kvs",
                     Json::array({Json{{"key", base64_encode(issuance_key)},
                                       {"value", base64_encode(issuance_value)}}})}}}},
             Json{{"response_range",
                   {{"kvs",
                     Json::array({Json{{"key", base64_encode(snapshot_key)},
                                       {"value", base64_encode(snapshot_value)}}})}}}},
         })},
    }.dump());
    const EtcdQueueStore store(test_options(), client);

    const auto result = store.issue_or_recover(QueueIssueRequest{
        .identity_jti = "0123456789abcdef0123456789abcdef",
        .issued_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'000'001}},
        .identity_expires_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'001'801}},
        .snapshot = snapshot_with_release_batch(),
    });

    EXPECT_EQ(result.status, QueueIssueStatus::Recovered);
    EXPECT_EQ(result.number, 9U);
    EXPECT_EQ(result.snapshot.next_number, 10U);
    ASSERT_EQ(client->calls().size(), 3U);
    EXPECT_EQ(client->calls()[2].first, "/v3/kv/txn");
}

TEST(QueueStoreTest, FailureBeforeCommitReturnsUnavailableWithoutNumber) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json{{"ID", "123"}}.dump());
    client->enqueue(std::nullopt);  // 原事务未得到应答
    client->enqueue(Json{
        {"succeeded", true},
        {"responses",
         Json::array({
             Json{{"response_range", {{"kvs", Json::array()}}}},
             Json{{"response_range", {{"kvs", Json::array()}}}},
         })},
    }.dump());
    const EtcdQueueStore store(test_options(), client);

    const auto result = store.issue_or_recover(QueueIssueRequest{
        .identity_jti = "0123456789abcdef0123456789abcdef",
        .issued_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'000'001}},
        .identity_expires_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'001'801}},
        .snapshot = snapshot_with_release_batch(),
    });

    EXPECT_EQ(result.status, QueueIssueStatus::Unavailable);
    EXPECT_EQ(result.number, 0U);
    EXPECT_EQ(result.snapshot.next_number, 1U);
}

TEST(QueueStoreTest, UnreadableAmbiguousCommitPoisonsFurtherIssuance) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json{{"ID", "123"}}.dump());
    client->enqueue(std::nullopt);  // txn 可能已提交，但应答丢失
    client->enqueue(std::nullopt);  // 线性一致回读也不可用
    const EtcdQueueStore store(test_options(), client);
    const QueueIssueRequest first{
        .identity_jti = "0123456789abcdef0123456789abcdef",
        .issued_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'000'001}},
        .identity_expires_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'001'801}},
        .snapshot = snapshot_with_release_batch(),
    };

    EXPECT_EQ(
        store.issue_or_recover(first).status,
        QueueIssueStatus::Unavailable);
    auto second = first;
    second.identity_jti = "fedcba9876543210fedcba9876543210";
    EXPECT_EQ(
        store.issue_or_recover(second).status,
        QueueIssueStatus::Unavailable);
    EXPECT_FALSE(store.save_snapshot(first.snapshot, 1'700'000'002));
    EXPECT_EQ(client->calls().size(), 3U);
}

TEST(QueueStoreTest, ReusesOneLeaseForSameExpiryBucket) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json{{"ID", "123"}}.dump());
    client->enqueue(Json{{"succeeded", true}}.dump());
    client->enqueue(Json{{"succeeded", true}}.dump());
    const EtcdQueueStore store(test_options(), client);
    const auto issued_at = std::chrono::system_clock::time_point{
        std::chrono::seconds{1'700'000'001}};
    const auto expires_at = std::chrono::system_clock::time_point{
        std::chrono::seconds{1'700'001'801}};

    const auto first = store.issue_or_recover(QueueIssueRequest{
        .identity_jti = "0123456789abcdef0123456789abcdef",
        .issued_at = issued_at,
        .identity_expires_at = expires_at,
        .snapshot = snapshot_with_release_batch(),
    });
    const auto second = store.issue_or_recover(QueueIssueRequest{
        .identity_jti = "fedcba9876543210fedcba9876543210",
        .issued_at = issued_at,
        .identity_expires_at = expires_at,
        .snapshot = first.snapshot,
    });

    EXPECT_EQ(second.status, QueueIssueStatus::Issued);
    ASSERT_EQ(client->calls().size(), 3U);
    EXPECT_EQ(client->calls()[0].first, "/v3/lease/grant");
    EXPECT_EQ(client->calls()[1].first, "/v3/kv/txn");
    EXPECT_EQ(client->calls()[2].first, "/v3/kv/txn");
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
    client->enqueue(Json{{"count", 0}}.dump());
    const EtcdQueueStore store(test_options(), client);
    const auto snapshot = store.load_snapshot();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(*snapshot, snapshot_with_release_batch());
    // 快照精确 key 读取不带 range_end；随后扫描 issuance 前缀校验。
    ASSERT_EQ(client->calls().size(), 2U);
    EXPECT_NE(
        client->calls()[0].second.find(base64_encode(snapshot_key)),
        std::string::npos);
    EXPECT_EQ(
        client->calls()[0].second.find("range_end"), std::string::npos);
    EXPECT_NE(
        client->calls()[1].second.find("range_end"), std::string::npos);
}

TEST(QueueStoreTest, LegacySnapshotRestoresAsExpiredPrefix) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(range_response(
        {R"({"released_number":5,"next_number":9,"admit_rate":100})"}));
    client->enqueue(Json{{"count", 0}}.dump());
    const EtcdQueueStore store(test_options(), client);
    const auto snapshot = store.load_snapshot();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->release_batches_pruned_through, 5U);
    EXPECT_TRUE(snapshot->release_batches.empty());
}

TEST(QueueStoreTest, LoadSnapshotValidatesIssuanceAcrossPages) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(range_response(
        {R"({"released_number":5,"next_number":10,"admit_rate":100})"}));
    const std::string issuance_key =
        "/realmmesh/queue/issuance/a55798dd7966ed4b45bd53202cf7a982";
    const std::string issuance_value =
        R"({"identity_expires_at":1700001801,"issued_at":1700000001,"number":9})";
    client->enqueue(Json{
        {"more", true},
        {"kvs",
         Json::array({Json{{"key", base64_encode(issuance_key)},
                           {"value", base64_encode(issuance_value)}}})},
    }.dump());
    client->enqueue(Json{{"count", 0}}.dump());
    const EtcdQueueStore store(test_options(), client);

    const auto snapshot = store.load_snapshot();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->next_number, 10U);
    ASSERT_EQ(client->calls().size(), 3U);
    EXPECT_NE(client->calls()[1].second.find("\"limit\":\"1024\""),
              std::string::npos);
    std::string next_key = issuance_key;
    next_key.push_back('\0');
    EXPECT_NE(client->calls()[2].second.find(base64_encode(next_key)),
              std::string::npos);
}

TEST(QueueStoreTest, LoadSnapshotReturnsNulloptWhenMissing) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json{{"count", 0}}.dump());
    client->enqueue(Json{{"count", 0}}.dump());
    const EtcdQueueStore store(test_options(), client);
    EXPECT_FALSE(store.load_snapshot().has_value());
}

TEST(QueueStoreTest, LoadSnapshotRejectsOrphanIssuanceMappings) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json{{"count", 0}}.dump());
    client->enqueue(range_response({R"({"number":1})"}));
    const EtcdQueueStore store(test_options(), client);
    EXPECT_THROW(store.load_snapshot(), std::runtime_error);
}

TEST(QueueStoreTest, LoadSnapshotFailsClosedOnUnavailableIssuanceState) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(range_response(
        {R"({"released_number":5,"next_number":9,"admit_rate":100})"}));
    client->enqueue(std::nullopt);
    const EtcdQueueStore store(test_options(), client);
    EXPECT_THROW(store.load_snapshot(), std::runtime_error);
}

TEST(QueueStoreTest, LoadSnapshotRejectsCorruptIssuanceMapping) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(range_response(
        {R"({"released_number":5,"next_number":9,"admit_rate":100})"}));
    client->enqueue(range_response(
        {R"({"identity_expires_at":1700001801,"issued_at":1700000001,"number":9})"}));
    const EtcdQueueStore store(test_options(), client);
    EXPECT_THROW(store.load_snapshot(), std::runtime_error);
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
