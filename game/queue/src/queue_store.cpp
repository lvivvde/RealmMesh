#include "realmmesh/game/queue/queue_store.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
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

/// range 请求:range_end 为空 = 精确单 key;返回解码后的 (key, value)
/// 序列。调用失败或应答不可解析返回 nullopt。
[[nodiscard]] std::optional<std::vector<std::pair<std::string, std::string>>>
range(
    cluster::IEtcdHttpClient& client,
    const std::string& key,
    const std::string& range_end) {
    Json request{{"key", base64_encode(key)}};
    if (!range_end.empty()) {
        request["range_end"] = base64_encode(range_end);
    }
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
    std::vector<std::pair<std::string, std::string>> kvs;
    const auto entries = response.find("kvs");
    if (entries == response.end()) {
        return kvs;
    }
    for (const auto& entry : *entries) {
        const auto key_text = base64_decode(entry.value("key", ""));
        const auto value_text = base64_decode(entry.value("value", ""));
        if (!key_text.has_value() || !value_text.has_value()) {
            return std::nullopt;
        }
        kvs.emplace_back(std::move(*key_text), std::move(*value_text));
    }
    return kvs;
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
    if (options_.budget_prefix.empty() ||
        options_.snapshot_key.empty()) {
        throw std::invalid_argument("invalid etcd queue store options");
    }
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
        return std::nullopt;
    }
    Json snapshot;
    try {
        snapshot = Json::parse(kvs->front().second);
    } catch (const Json::exception&) {
        throw std::runtime_error("queue snapshot load failed: corrupt value");
    }
    const auto released = budget_integer(snapshot, "released_number");
    const auto next = budget_integer(snapshot, "next_number");
    const auto rate = budget_integer(snapshot, "admit_rate");
    if (!released.has_value() || !next.has_value() || !rate.has_value() ||
        *released >= *next) {
        throw std::runtime_error(
            "queue snapshot load failed: corrupt snapshot");
    }
    QueueSnapshot result{
        .released_number = *released,
        .next_number = *next,
        .admit_rate = *rate,
    };
    const auto pruned = snapshot.find("release_batches_pruned_through");
    const auto batches = snapshot.find("release_batches");
    if (pruned == snapshot.end() && batches == snapshot.end()) {
        // #84 以前的快照没有 release time。它只能作为已经过期的前缀
        // 恢复，绝不能根据重启时间推断新窗口。
        result.release_batches_pruned_through = *released;
    } else {
        if (pruned == snapshot.end() || batches == snapshot.end() ||
            !batches->is_array()) {
            throw std::runtime_error(
                "queue snapshot load failed: corrupt release ledger");
        }
        const auto pruned_value =
            budget_integer(snapshot, "release_batches_pruned_through");
        if (!pruned_value.has_value()) {
            throw std::runtime_error(
                "queue snapshot load failed: corrupt release ledger");
        }
        result.release_batches_pruned_through = *pruned_value;
        result.release_batches.reserve(batches->size());
        for (const auto& batch : *batches) {
            if (!batch.is_object() || batch.size() != 3U) {
                throw std::runtime_error(
                    "queue snapshot load failed: corrupt release batch");
            }
            const auto first = budget_integer(batch, "first_number");
            const auto last = budget_integer(batch, "last_number");
            const auto released_at = snapshot_time(batch, "released_at");
            if (!first.has_value() || !last.has_value() ||
                !released_at.has_value()) {
                throw std::runtime_error(
                    "queue snapshot load failed: corrupt release batch");
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
        throw std::runtime_error(
            "queue snapshot load failed: inconsistent release ledger");
    }
    return result;
}

bool EtcdQueueStore::save_snapshot(
    const QueueSnapshot& snapshot, std::int64_t updated_at_seconds) const {
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
    const Json value{
        {"released_number", snapshot.released_number},
        {"next_number", snapshot.next_number},
        {"admit_rate", snapshot.admit_rate},
        {"release_batches_pruned_through",
         snapshot.release_batches_pruned_through},
        {"release_batches", std::move(release_batches)},
        {"updated_at", updated_at_seconds},
    };
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
