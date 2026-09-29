#include "realmmesh/game/queue/queue_store.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace realm::game::queue {
namespace {

using Json = nlohmann::json;

// base64 编解码与前缀 range_end 推导,与 etcd_service_registry 的同名
// 文件内助手重复:注册中心未导出,暂不跨模块拉公共头(先例见 #41 裁决)。

std::string base64_encode(std::string_view input) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2U) / 3U) * 4U);
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

std::optional<std::string> base64_decode(std::string_view input) {
    auto decode = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') return character - 'A';
        if (character >= 'a' && character <= 'z') return character - 'a' + 26;
        if (character >= '0' && character <= '9') return character - '0' + 52;
        if (character == '+') return 62;
        if (character == '/') return 63;
        return -1;
    };
    if (input.size() % 4U != 0U) return std::nullopt;
    std::string output;
    output.reserve((input.size() / 4U) * 3U);
    for (std::size_t offset = 0; offset < input.size(); offset += 4U) {
        const int first = decode(input[offset]);
        const int second = decode(input[offset + 1U]);
        const int third =
            input[offset + 2U] == '=' ? 0 : decode(input[offset + 2U]);
        const int fourth =
            input[offset + 3U] == '=' ? 0 : decode(input[offset + 3U]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0) {
            return std::nullopt;
        }
        const auto value = (static_cast<std::uint32_t>(first) << 18U) |
                           (static_cast<std::uint32_t>(second) << 12U) |
                           (static_cast<std::uint32_t>(third) << 6U) |
                           static_cast<std::uint32_t>(fourth);
        output.push_back(static_cast<char>((value >> 16U) & 0xFFU));
        if (input[offset + 2U] != '=') {
            output.push_back(static_cast<char>((value >> 8U) & 0xFFU));
        }
        if (input[offset + 3U] != '=') {
            output.push_back(static_cast<char>(value & 0xFFU));
        }
    }
    return output;
}

std::string prefix_range_end(std::string prefix) {
    for (auto iterator = prefix.rbegin(); iterator != prefix.rend();
         ++iterator) {
        const auto value = static_cast<unsigned char>(*iterator);
        if (value != 0xFFU) {
            *iterator = static_cast<char>(value + 1U);
            prefix.erase(iterator.base(), prefix.end());
            return prefix;
        }
    }
    return std::string(1, '\0');
}

struct RangePage final {
    std::vector<std::pair<std::string, std::string>> entries;
    bool more{false};
};

/// range 请求:range_end 为空 = 精确单 key；limit=0 = 不分页。调用失败
/// 或应答不可解析返回 nullopt。
[[nodiscard]] std::optional<RangePage> range_page(
    cluster::IEtcdHttpClient& client,
    const std::string& key,
    const std::string& range_end,
    std::uint64_t limit = 0) {
    Json request{{"key", base64_encode(key)}};
    if (!range_end.empty()) {
        request["range_end"] = base64_encode(range_end);
    }
    if (limit > 0U) request["limit"] = std::to_string(limit);
    const auto body = client.post("/v3/kv/range", request.dump(), nullptr);
    if (!body.has_value()) {
        return std::nullopt;
    }
    Json response;
    try {
        response = Json::parse(*body);
    } catch (const Json::exception&) {
        return std::nullopt;
    }
    if (!response.is_object() || response.contains("error")) {
        return std::nullopt;
    }
    RangePage page;
    const auto more = response.find("more");
    if (more != response.end()) {
        if (!more->is_boolean()) return std::nullopt;
        page.more = more->get<bool>();
    }
    const auto entries = response.find("kvs");
    if (entries == response.end()) {
        if (page.more) return std::nullopt;
        return page;
    }
    if (!entries->is_array()) return std::nullopt;
    for (const auto& entry : *entries) {
        if (!entry.is_object()) return std::nullopt;
        const auto key_text = base64_decode(entry.value("key", ""));
        const auto value_text = base64_decode(entry.value("value", ""));
        if (!key_text.has_value() || !value_text.has_value()) {
            return std::nullopt;
        }
        page.entries.emplace_back(
            std::move(*key_text), std::move(*value_text));
    }
    return page;
}

[[nodiscard]] std::optional<std::vector<std::pair<std::string, std::string>>>
range(
    cluster::IEtcdHttpClient& client,
    const std::string& key,
    const std::string& range_end) {
    auto page = range_page(client, key, range_end);
    if (!page.has_value() || page->more) return std::nullopt;
    return std::move(page->entries);
}

/// 额度整数字段:接受 JSON 无符号整数或十进制字符串(与注册中心同
/// 宽容度);缺失/负数/非整返回 nullopt。
[[nodiscard]] std::optional<std::uint64_t> budget_integer(
    const Json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end()) {
        return std::nullopt;
    }
    if (found->is_number_unsigned()) {
        return found->get<std::uint64_t>();
    }
    if (!found->is_string()) {
        return std::nullopt;  // 含符号整数(负数)一律不认
    }
    const auto text = found->get<std::string>();
    if (text.empty() || text.front() == '-') {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        const auto value = static_cast<std::uint64_t>(digit - '0');
        if (result >
            (std::numeric_limits<std::uint64_t>::max() - value) / 10U) {
            return std::nullopt;
        }
        result = result * 10U + value;
    }
    return result;
}

[[nodiscard]] std::optional<std::int64_t> snapshot_time(
    const Json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end()) return std::nullopt;
    if (found->is_number_unsigned()) {
        const auto parsed = found->get<std::uint64_t>();
        if (parsed > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(parsed);
    }
    if (!found->is_number_integer()) return std::nullopt;
    const auto parsed = found->get<std::int64_t>();
    return parsed < 0 ? std::nullopt
                      : std::optional<std::int64_t>{parsed};
}

[[nodiscard]] Json snapshot_json(
    const QueueSnapshot& snapshot, std::int64_t updated_at_seconds) {
    snapshot.validate();
    Json release_batches = Json::array();
    for (const auto& batch : snapshot.release_batches) {
        release_batches.push_back(Json{
            {"first_number", batch.first_number},
            {"last_number", batch.last_number},
            {"released_at",
             std::chrono::duration_cast<std::chrono::seconds>(
                 batch.released_at.time_since_epoch())
                 .count()},
        });
    }
    return Json{
        {"released_number", snapshot.released_number},
        {"next_number", snapshot.next_number},
        {"admit_rate", snapshot.admit_rate},
        {"release_batches_pruned_through",
         snapshot.release_batches_pruned_through},
        {"release_batches", std::move(release_batches)},
        {"updated_at", updated_at_seconds},
    };
}

[[nodiscard]] std::optional<QueueSnapshot> decode_snapshot_value(
    std::string_view encoded) {
    Json snapshot;
    try {
        snapshot = Json::parse(encoded);
    } catch (const Json::exception&) {
        return std::nullopt;
    }
    const auto released = budget_integer(snapshot, "released_number");
    const auto next = budget_integer(snapshot, "next_number");
    const auto rate = budget_integer(snapshot, "admit_rate");
    if (!released.has_value() || !next.has_value() || !rate.has_value() ||
        *released >= *next) {
        return std::nullopt;
    }
    QueueSnapshot result{
        .released_number = *released,
        .next_number = *next,
        .admit_rate = *rate,
    };
    const auto pruned = snapshot.find("release_batches_pruned_through");
    const auto batches = snapshot.find("release_batches");
    if (pruned == snapshot.end() && batches == snapshot.end()) {
        result.release_batches_pruned_through = *released;
    } else {
        if (pruned == snapshot.end() || batches == snapshot.end() ||
            !batches->is_array()) {
            return std::nullopt;
        }
        const auto pruned_value =
            budget_integer(snapshot, "release_batches_pruned_through");
        if (!pruned_value.has_value()) return std::nullopt;
        result.release_batches_pruned_through = *pruned_value;
        result.release_batches.reserve(batches->size());
        for (const auto& batch : *batches) {
            if (!batch.is_object() || batch.size() != 3U) return std::nullopt;
            const auto first = budget_integer(batch, "first_number");
            const auto last = budget_integer(batch, "last_number");
            const auto released_at = snapshot_time(batch, "released_at");
            if (!first.has_value() || !last.has_value() ||
                !released_at.has_value()) {
                return std::nullopt;
            }
            result.release_batches.push_back(QueueReleaseBatch{
                .first_number = *first,
                .last_number = *last,
                .released_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{*released_at}},
            });
        }
    }
    try {
        result.validate();
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<std::string> transaction_range_value(
    const Json& response,
    std::size_t index,
    std::string_view expected_key) {
    const auto responses = response.find("responses");
    if (responses == response.end() || !responses->is_array() ||
        responses->size() <= index) {
        return std::nullopt;
    }
    const auto range_response = responses->at(index).find("response_range");
    if (range_response == responses->at(index).end() ||
        !range_response->is_object()) {
        return std::nullopt;
    }
    const auto kvs = range_response->find("kvs");
    if (kvs == range_response->end() || !kvs->is_array() ||
        kvs->size() != 1U) {
        return std::nullopt;
    }
    const auto& entry = kvs->front();
    const auto key = base64_decode(entry.value("key", ""));
    const auto value = base64_decode(entry.value("value", ""));
    if (!key.has_value() || *key != expected_key || !value.has_value()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<QueueIssueResult> recovered_issue_result(
    const Json& response,
    std::string_view issuance_key,
    std::string_view snapshot_key,
    std::int64_t expected_identity_expires_at) {
    const auto issuance = transaction_range_value(response, 0, issuance_key);
    const auto snapshot = transaction_range_value(response, 1, snapshot_key);
    if (!issuance.has_value() || !snapshot.has_value()) return std::nullopt;
    const auto issuance_json = Json::parse(*issuance);
    if (!issuance_json.is_object() || issuance_json.size() != 3U) {
        return std::nullopt;
    }
    const auto recovered_number = budget_integer(issuance_json, "number");
    const auto recovered_issued_at =
        snapshot_time(issuance_json, "issued_at");
    const auto recovered_expires_at =
        snapshot_time(issuance_json, "identity_expires_at");
    const auto recovered_snapshot = decode_snapshot_value(*snapshot);
    if (!recovered_number.has_value() || !recovered_issued_at.has_value() ||
        !recovered_expires_at.has_value() ||
        *recovered_expires_at != expected_identity_expires_at ||
        *recovered_expires_at <= *recovered_issued_at ||
        !recovered_snapshot.has_value() ||
        recovered_snapshot->next_number <= *recovered_number) {
        return std::nullopt;
    }
    return QueueIssueResult{
        .status = QueueIssueStatus::Recovered,
        .number = *recovered_number,
        .issued_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{*recovered_issued_at}},
        .snapshot = std::move(*recovered_snapshot),
    };
}

[[nodiscard]] std::string issuance_digest(std::string_view identity_jti) {
    if (sodium_init() < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
    std::array<unsigned char, 16> digest{};
    if (crypto_generichash(
            digest.data(), digest.size(),
            reinterpret_cast<const unsigned char*>(identity_jti.data()),
            identity_jti.size(), nullptr, 0) != 0) {
        throw std::runtime_error("failed to derive queue issuance digest");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        encoded.push_back(hex[byte >> 4U]);
        encoded.push_back(hex[byte & 0x0FU]);
    }
    return encoded;
}

[[nodiscard]] std::int64_t epoch_seconds(
    std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               value.time_since_epoch())
        .count();
}

void validate_issuance_state(
    cluster::IEtcdHttpClient& client,
    std::string_view issuance_prefix,
    const QueueSnapshot& snapshot) {
    const std::string key_prefix = std::string{issuance_prefix} + "/";
    const std::string range_end = prefix_range_end(key_prefix);
    std::string range_start = key_prefix;
    std::unordered_set<std::uint64_t> active_numbers;
    static constexpr std::uint64_t page_size = 1024;
    while (true) {
        auto page = range_page(
            client, range_start, range_end, page_size);
        if (!page.has_value()) {
            throw std::runtime_error(
                "queue issuance state check failed: etcd unavailable");
        }
        for (const auto& [key, value] : page->entries) {
            const std::string_view suffix =
                std::string_view{key}.substr(
                    std::min(key.size(), key_prefix.size()));
            const bool valid_key = key.starts_with(key_prefix) &&
                                   suffix.size() == 32U &&
                                   std::all_of(
                                       suffix.begin(),
                                       suffix.end(),
                                       [](char digit) {
                                           return (digit >= '0' &&
                                                   digit <= '9') ||
                                                  (digit >= 'a' &&
                                                   digit <= 'f');
                                       });
            if (!valid_key) {
                throw std::runtime_error(
                    "queue issuance state check failed: corrupt key");
            }

            try {
                const auto record = Json::parse(value);
                const auto number = budget_integer(record, "number");
                const auto issued_at = snapshot_time(record, "issued_at");
                const auto expires_at =
                    snapshot_time(record, "identity_expires_at");
                if (!record.is_object() || record.size() != 3U ||
                    !number.has_value() || *number == 0U ||
                    *number >= snapshot.next_number ||
                    !issued_at.has_value() || !expires_at.has_value() ||
                    *expires_at <= *issued_at ||
                    !active_numbers.insert(*number).second) {
                    throw std::runtime_error(
                        "queue issuance state check failed: corrupt mapping");
                }
            } catch (const Json::exception&) {
                throw std::runtime_error(
                    "queue issuance state check failed: corrupt mapping");
            }
        }
        if (!page->more) return;
        if (page->entries.empty()) {
            throw std::runtime_error(
                "queue issuance state check failed: invalid pagination");
        }
        range_start = page->entries.back().first;
        range_start.push_back('\0');
    }
}

}  // namespace

EtcdQueueStore::EtcdQueueStore(
    Options options, std::shared_ptr<cluster::IEtcdHttpClient> client)
    : options_(std::move(options)),
      client_(std::move(client)) {
    if (client_ == nullptr) {
        throw std::invalid_argument("etcd queue store requires a client");
    }
    while (options_.budget_prefix.size() > 1U &&
           options_.budget_prefix.back() == '/') {
        options_.budget_prefix.pop_back();
    }
    while (options_.issuance_prefix.size() > 1U &&
           options_.issuance_prefix.back() == '/') {
        options_.issuance_prefix.pop_back();
    }
    if (options_.budget_prefix.empty() ||
        options_.snapshot_key.empty() || options_.issuance_prefix.empty()) {
        throw std::invalid_argument("invalid etcd queue store options");
    }
}

QueueIssueResult EtcdQueueStore::issue_or_recover(
    const QueueIssueRequest& request) const {
    if (issuance_state_uncertain_) return {};
    request.snapshot.validate();
    const auto issued_at = epoch_seconds(request.issued_at);
    const auto expires_at = epoch_seconds(request.identity_expires_at);
    if (request.identity_jti.empty() || issued_at < 0 ||
        expires_at <= issued_at ||
        request.snapshot.next_number ==
            std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument("invalid queue issuance request");
    }

    static constexpr std::int64_t lease_bucket_seconds = 60;
    const auto lease_bucket =
        ((expires_at + lease_bucket_seconds - 1) / lease_bucket_seconds) *
        lease_bucket_seconds;
    std::erase_if(issuance_leases_, [issued_at](const auto& entry) {
        return entry.first <= issued_at;
    });
    auto lease = issuance_leases_.find(lease_bucket);
    if (lease == issuance_leases_.end()) {
        const auto ttl = lease_bucket - issued_at;
        const auto response = client_->post(
            "/v3/lease/grant",
            Json{{"TTL", std::to_string(ttl)}}.dump(), nullptr);
        if (!response.has_value()) return {};
        try {
            const auto body = Json::parse(*response);
            const auto id = budget_integer(body, "ID");
            if (!id.has_value() || *id == 0 ||
                *id > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max())) {
                return {};
            }
            lease = issuance_leases_
                        .emplace(lease_bucket, static_cast<std::int64_t>(*id))
                        .first;
        } catch (const Json::exception&) {
            return {};
        }
    }

    const auto number = request.snapshot.next_number;
    auto committed_snapshot = request.snapshot;
    ++committed_snapshot.next_number;
    const auto snapshot_value =
        snapshot_json(committed_snapshot, issued_at).dump();
    const auto issuance_key =
        options_.issuance_prefix + "/" + issuance_digest(request.identity_jti);
    const auto issuance_value = Json{
        {"number", number},
        {"issued_at", issued_at},
        {"identity_expires_at", expires_at},
    }.dump();
    const Json transaction{
        {"compare",
         Json::array({Json{{"key", base64_encode(issuance_key)},
                           {"target", "VERSION"},
                           {"result", "EQUAL"},
                           {"version", 0}}})},
        {"success",
         Json::array({
             Json{{"requestPut",
                   {{"key", base64_encode(options_.snapshot_key)},
                    {"value", base64_encode(snapshot_value)}}}},
             Json{{"requestPut",
                   {{"key", base64_encode(issuance_key)},
                    {"value", base64_encode(issuance_value)},
                    {"lease", std::to_string(lease->second)}}}},
         })},
        {"failure",
         Json::array({
             Json{{"requestRange", {{"key", base64_encode(issuance_key)}}}},
             Json{{"requestRange",
                   {{"key", base64_encode(options_.snapshot_key)}}}},
         })},
    };
    auto response = client_->post(
        "/v3/kv/txn", transaction.dump(), nullptr);
    const auto fail_uncertain = [this]() {
        issuance_state_uncertain_ = true;
        return QueueIssueResult{};
    };
    if (!response.has_value()) {
        const Json read_back{
            {"success",
             Json::array({
                 Json{{"requestRange", {{"key", base64_encode(issuance_key)}}}},
                 Json{{"requestRange",
                       {{"key", base64_encode(options_.snapshot_key)}}}},
             })},
        };
        response = client_->post(
            "/v3/kv/txn", read_back.dump(), nullptr);
        if (!response.has_value()) return fail_uncertain();
        try {
            const auto recovered = recovered_issue_result(
                Json::parse(*response), issuance_key, options_.snapshot_key,
                expires_at);
            if (recovered.has_value()) return *recovered;
            return fail_uncertain();
        } catch (const Json::exception&) {
            return fail_uncertain();
        }
    }
    try {
        const auto body = Json::parse(*response);
        const auto succeeded = body.find("succeeded");
        if (succeeded != body.end() && !succeeded->is_boolean()) {
            return fail_uncertain();
        }
        // etcd JSON gateway omits the protobuf default `false`; missing is a
        // compare miss, not a malformed response.
        if (succeeded == body.end() || !succeeded->get<bool>()) {
            const auto recovered = recovered_issue_result(
                body, issuance_key, options_.snapshot_key, expires_at);
            if (recovered.has_value()) return *recovered;
            return fail_uncertain();
        }
    } catch (const Json::exception&) {
        return fail_uncertain();
    }
    return {
        .status = QueueIssueStatus::Issued,
        .number = number,
        .issued_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{issued_at}},
        .snapshot = std::move(committed_snapshot),
    };
}

std::optional<BudgetAggregate> EtcdQueueStore::refresh_budgets() const {
    const auto gateway_prefix = options_.budget_prefix + "/gateway/";
    const auto realm_prefix = options_.budget_prefix + "/realm/";

    // 任一前缀不可达即整体失败:本批停放优于用过期/残缺额度过量放行。
    // 前缀查询:range_end 取前缀上界,空应答与失败同样 fail-closed。
    const auto gateway_kvs =
        range(*client_, gateway_prefix, prefix_range_end(gateway_prefix));
    const auto realm_kvs =
        range(*client_, realm_prefix, prefix_range_end(realm_prefix));
    if (!gateway_kvs.has_value() || !realm_kvs.has_value()) {
        return std::nullopt;
    }

    std::vector<GatewayBudget> gateways;
    gateways.reserve(gateway_kvs->size());
    for (const auto& [key, value] : *gateway_kvs) {
        Json budget;
        try {
            budget = Json::parse(value);
        } catch (const Json::exception&) {
            continue;  // 损坏条目跳过,可用性优先
        }
        const auto conn_free = budget_integer(budget, "conn_free");
        const auto fetch_free = budget_integer(budget, "fetch_free");
        if (!conn_free.has_value() || !fetch_free.has_value()) {
            continue;
        }
        gateways.push_back({*conn_free, *fetch_free});
    }
    std::vector<RealmBudget> realms;
    realms.reserve(realm_kvs->size());
    for (const auto& [key, value] : *realm_kvs) {
        Json budget;
        try {
            budget = Json::parse(value);
        } catch (const Json::exception&) {
            continue;
        }
        const auto conn_free = budget_integer(budget, "conn_free");
        if (!conn_free.has_value()) {
            continue;
        }
        realms.push_back({*conn_free});
    }
    return aggregate_budgets(gateways, realms);
}

std::optional<QueueSnapshot> EtcdQueueStore::load_snapshot() const {
    // 精确 key 读取(range_end 为空)。三态契约:无快照返回 nullopt
    // (确属空状态,从零开始);etcd 不可达或快照损坏抛出——冷备未知
    // 时从零重发会与存量号牌冲突,启动必须失败(fail-closed)。
    const auto kvs = range(*client_, options_.snapshot_key, "");
    if (!kvs.has_value()) {
        throw std::runtime_error(
            "queue snapshot load failed: etcd unavailable");
    }
    if (kvs->empty()) {
        // snapshot 与 issuance 必须成对为空。只剩映射意味着权威状态被
        // 部分删除；若从 next_number=1 启动，另一身份会复用存量号码。
        const auto issuance = range_page(
            *client_, options_.issuance_prefix + "/",
            prefix_range_end(options_.issuance_prefix + "/"), 1);
        if (!issuance.has_value()) {
            throw std::runtime_error(
                "queue issuance state check failed: etcd unavailable");
        }
        if (!issuance->entries.empty()) {
            throw std::runtime_error(
                "queue snapshot load failed: orphan issuance mappings");
        }
        return std::nullopt;
    }
    const auto snapshot = decode_snapshot_value(kvs->front().second);
    if (!snapshot.has_value()) {
        throw std::runtime_error("queue snapshot load failed: corrupt snapshot");
    }
    validate_issuance_state(*client_, options_.issuance_prefix, *snapshot);
    return snapshot;
}

bool EtcdQueueStore::save_snapshot(
    const QueueSnapshot& snapshot, std::int64_t updated_at_seconds) const {
    if (issuance_state_uncertain_) return false;
    const auto value = snapshot_json(snapshot, updated_at_seconds);
    const auto body = client_->post(
        "/v3/kv/put",
        Json{
            {"key", base64_encode(options_.snapshot_key)},
            {"value", base64_encode(value.dump())},
        }
            .dump(),
        nullptr);
    return body.has_value();
}

}  // namespace realm::game::queue
