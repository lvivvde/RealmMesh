#include "realmmesh/game/gateway/admission_consumption_store.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realm::game::gateway {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

constexpr std::string_view identity_jti =
    "000102030405060708090a0b0c0d0e0f";
constexpr std::string_view grant_jti =
    "101112131415161718191a1b1c1d1e1f";
constexpr std::string_view digest_key_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";

std::chrono::system_clock::time_point at(std::int64_t seconds) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
}

AdmissionConsumptionOptions options() {
    return {
        .key_prefix = "/realmmesh/admission/test",
        .reservation_ttl = 10s,
        .digest_key =
            parse_admission_consumption_digest_key(digest_key_hex),
    };
}

AdmissionReserveRequest request(
    std::string owner, std::int64_t now = 100) {
    return {
        .identity_jti = std::string{identity_jti},
        .grant_jti = std::string{grant_jti},
        .owner = std::move(owner),
        .now = at(now),
        .consume_until = at(now > 400 ? now + 100 : 500),
    };
}

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

class ScriptedEtcdClient final : public cluster::IEtcdHttpClient {
public:
    void enqueue(std::optional<std::string> response) {
        responses_.push_back(std::move(response));
    }

    [[nodiscard]] std::optional<std::string> post(
        std::string_view path,
        std::string_view json_body,
        std::string* error) override {
        static_cast<void>(error);
        calls_.emplace_back(path, json_body);
        if (responses_.empty()) return std::nullopt;
        auto response = std::move(responses_.front());
        responses_.pop_front();
        return response;
    }

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>&
    calls() const noexcept {
        return calls_;
    }

private:
    std::deque<std::optional<std::string>> responses_;
    std::vector<std::pair<std::string, std::string>> calls_;
};

TEST(AdmissionConsumptionOptionsTest, RejectsUnsafeConfiguration) {
    auto invalid = options();
    invalid.reservation_ttl = 0s;
    EXPECT_THROW(invalid.validate(), std::invalid_argument);
    invalid = options();
    invalid.digest_key = {};
    EXPECT_THROW(invalid.validate(), std::invalid_argument);
    invalid = options();
    invalid.key_prefix = "relative";
    EXPECT_THROW(invalid.validate(), std::invalid_argument);
    invalid = options();
    invalid.key_prefix = "/bad prefix";
    EXPECT_THROW(invalid.validate(), std::invalid_argument);
}

TEST(InMemoryAdmissionConsumptionStoreTest, ImplementsFencedTwoPhaseContract) {
    InMemoryAdmissionConsumptionStore store(options());

    const auto first = store.reserve(request("gateway-a/attempt-1"));
    ASSERT_EQ(first.status, AdmissionReserveStatus::Reserved);
    ASSERT_TRUE(first.reservation.has_value());
    EXPECT_EQ(first.reservation->fencing, 1U);

    const auto replay = store.reserve(request("gateway-a/attempt-1", 101));
    ASSERT_EQ(replay.status, AdmissionReserveStatus::Reserved);
    ASSERT_TRUE(replay.reservation.has_value());
    EXPECT_EQ(*replay.reservation, *first.reservation);

    EXPECT_EQ(
        store.reserve(request("gateway-b/attempt-1", 101)).status,
        AdmissionReserveStatus::InProgress);
    EXPECT_EQ(
        store.release(*first.reservation, at(102)),
        AdmissionMutationStatus::Applied);

    const auto second = store.reserve(request("gateway-b/attempt-1", 102));
    ASSERT_EQ(second.status, AdmissionReserveStatus::Reserved);
    ASSERT_TRUE(second.reservation.has_value());
    EXPECT_EQ(second.reservation->fencing, 2U);
    EXPECT_EQ(
        store.commit(*first.reservation, at(103)),
        AdmissionMutationStatus::Lost);
    EXPECT_EQ(
        store.release(*first.reservation, at(103)),
        AdmissionMutationStatus::Lost);
    EXPECT_EQ(
        store.commit(*second.reservation, at(103)),
        AdmissionMutationStatus::Applied);
    EXPECT_EQ(
        store.commit(*second.reservation, at(104)),
        AdmissionMutationStatus::Applied);
    EXPECT_EQ(
        store.reserve(request("gateway-c/attempt-1", 500)).status,
        AdmissionReserveStatus::Consumed);

    const auto after_retention =
        store.reserve(request("gateway-c/attempt-1", 501));
    ASSERT_EQ(after_retention.status, AdmissionReserveStatus::Reserved);
    ASSERT_TRUE(after_retention.reservation.has_value());
    EXPECT_EQ(after_retention.reservation->fencing, 3U);
}

TEST(InMemoryAdmissionConsumptionStoreTest, LeaseExpiryAndOutageFailClosed) {
    InMemoryAdmissionConsumptionStore store(options());
    const auto first = store.reserve(request("gateway-a/attempt-1"));
    ASSERT_TRUE(first.reservation.has_value());

    const auto replacement =
        store.reserve(request("gateway-b/attempt-1", 110));
    ASSERT_EQ(replacement.status, AdmissionReserveStatus::Reserved);
    ASSERT_TRUE(replacement.reservation.has_value());
    EXPECT_EQ(replacement.reservation->fencing, 2U);
    EXPECT_EQ(
        store.commit(*first.reservation, at(110)),
        AdmissionMutationStatus::Lost);

    store.set_available(false);
    EXPECT_FALSE(store.available());
    EXPECT_EQ(
        store.reserve(request("gateway-c/attempt-1", 120)).status,
        AdmissionReserveStatus::Unavailable);
    EXPECT_EQ(
        store.release(*replacement.reservation, at(120)),
        AdmissionMutationStatus::Unavailable);
    store.set_available(true);
    EXPECT_TRUE(store.available());

    InMemoryAdmissionConsumptionStore deadline_store(options());
    auto expiring = request("gateway-a/attempt-2");
    expiring.consume_until = at(105);
    const auto short_chain = deadline_store.reserve(expiring);
    ASSERT_TRUE(short_chain.reservation.has_value());
    EXPECT_EQ(
        deadline_store.commit(*short_chain.reservation, at(106)),
        AdmissionMutationStatus::Lost);
}

TEST(EtcdAdmissionConsumptionStoreTest, UsesLinearizableRangeAndCasWithDigests) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json::object().dump());
    client->enqueue(Json{{"ID", "11"}, {"TTL", "401"}}.dump());
    client->enqueue(Json{{"succeeded", true}}.dump());
    EtcdAdmissionConsumptionStore store(options(), client);

    const auto reserved = store.reserve(request("gateway-a/attempt-1"));
    ASSERT_EQ(reserved.status, AdmissionReserveStatus::Reserved);
    ASSERT_TRUE(reserved.reservation.has_value());
    ASSERT_EQ(client->calls().size(), 3U);
    EXPECT_EQ(client->calls()[0].first, "/v3/kv/range");
    EXPECT_EQ(client->calls()[0].second.find("serializable"),
              std::string::npos);
    EXPECT_EQ(client->calls()[1].first, "/v3/lease/grant");
    EXPECT_EQ(Json::parse(client->calls()[1].second)["TTL"], "401");
    EXPECT_EQ(client->calls()[2].first, "/v3/kv/txn");
    EXPECT_EQ(client->calls()[2].second.find(identity_jti), std::string::npos);
    EXPECT_EQ(client->calls()[2].second.find(grant_jti), std::string::npos);

    const auto transaction = Json::parse(client->calls()[2].second);
    ASSERT_EQ(transaction["compare"].size(), 1U);
    EXPECT_EQ(transaction["compare"][0]["target"], "VERSION");
    EXPECT_EQ(transaction["compare"][0]["version"], 0);

    const auto encoded_key =
        transaction["success"][0]["requestPut"]["key"].get<std::string>();
    const auto encoded_value =
        transaction["success"][0]["requestPut"]["value"].get<std::string>();
    EXPECT_EQ(transaction["success"][0]["requestPut"]["lease"], "11");
    const auto decoded_key = base64_decode(encoded_key);
    const auto decoded_value = base64_decode(encoded_value);
    ASSERT_TRUE(decoded_key.has_value());
    ASSERT_TRUE(decoded_value.has_value());
    EXPECT_EQ(decoded_key->find(identity_jti), std::string::npos);
    EXPECT_EQ(decoded_value->find(identity_jti), std::string::npos);
    EXPECT_EQ(decoded_value->find(grant_jti), std::string::npos);
    client->enqueue(
        Json{{"kvs",
              Json::array({Json{{"key", encoded_key},
                                {"value", encoded_value},
                                {"mod_revision", "7"},
                                {"lease", "11"}}})}}
            .dump());
    client->enqueue(Json{{"succeeded", true}}.dump());

    EXPECT_EQ(
        store.commit(*reserved.reservation, at(101)),
        AdmissionMutationStatus::Applied);
    ASSERT_EQ(client->calls().size(), 5U);
    const auto commit = Json::parse(client->calls()[4].second);
    EXPECT_EQ(commit["compare"][0]["target"], "MOD");
    EXPECT_EQ(commit["compare"][0]["mod_revision"], 7);
    const auto committed_value = base64_decode(
        commit["success"][0]["requestPut"]["value"].get<std::string>());
    EXPECT_EQ(commit["success"][0]["requestPut"]["lease"], "11");
    ASSERT_TRUE(committed_value.has_value());
    EXPECT_EQ(Json::parse(*committed_value)["state"], "consumed");
}

TEST(EtcdAdmissionConsumptionStoreTest, RetriesCasAndFencesReleasedOwner) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json::object().dump());
    client->enqueue(Json{{"ID", "21"}, {"TTL", "401"}}.dump());
    client->enqueue(Json{{"succeeded", false}}.dump());
    client->enqueue(Json::object().dump());
    client->enqueue(Json{{"succeeded", true}}.dump());
    EtcdAdmissionConsumptionStore store(options(), client);

    const auto first = store.reserve(request("gateway-a/attempt-1"));
    ASSERT_EQ(first.status, AdmissionReserveStatus::Reserved);
    ASSERT_TRUE(first.reservation.has_value());
    ASSERT_EQ(client->calls().size(), 5U);

    const auto create = Json::parse(client->calls()[4].second);
    const auto encoded_key =
        create["success"][0]["requestPut"]["key"].get<std::string>();
    const auto first_value =
        create["success"][0]["requestPut"]["value"].get<std::string>();
    client->enqueue(
        Json{{"kvs",
              Json::array({Json{{"key", encoded_key},
                                {"value", first_value},
                                {"mod_revision", "7"},
                                {"lease", "21"}}})}}
            .dump());
    client->enqueue(Json{{"succeeded", true}}.dump());

    EXPECT_EQ(
        store.release(*first.reservation, at(102)),
        AdmissionMutationStatus::Applied);
    ASSERT_EQ(client->calls().size(), 7U);
    const auto released = Json::parse(client->calls()[6].second);
    const auto released_value =
        released["success"][0]["requestPut"]["value"].get<std::string>();
    EXPECT_EQ(released["success"][0]["requestPut"]["lease"], "21");
    client->enqueue(
        Json{{"kvs",
              Json::array({Json{{"key", encoded_key},
                                {"value", released_value},
                                {"mod_revision", "8"},
                                {"lease", "21"}}})}}
            .dump());
    client->enqueue(Json{{"ID", "22"}, {"TTL", "399"}}.dump());
    client->enqueue(Json{{"succeeded", true}}.dump());

    const auto replacement =
        store.reserve(request("gateway-b/attempt-1", 102));
    ASSERT_EQ(replacement.status, AdmissionReserveStatus::Reserved);
    ASSERT_TRUE(replacement.reservation.has_value());
    EXPECT_EQ(replacement.reservation->fencing, 2U);
    ASSERT_EQ(client->calls().size(), 10U);
    const auto replaced = Json::parse(client->calls()[9].second);
    const auto replaced_value =
        replaced["success"][0]["requestPut"]["value"].get<std::string>();
    EXPECT_EQ(replaced["success"][0]["requestPut"]["lease"], "22");
    client->enqueue(
        Json{{"kvs",
              Json::array({Json{{"key", encoded_key},
                                {"value", replaced_value},
                                {"mod_revision", "9"},
                                {"lease", "22"}}})}}
            .dump());

    EXPECT_EQ(
        store.commit(*first.reservation, at(103)),
        AdmissionMutationStatus::Lost);
    EXPECT_EQ(client->calls().size(), 11U);
}

TEST(EtcdAdmissionConsumptionStoreTest, RejectsMismatchedReturnedKey) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(
        Json{{"kvs",
              Json::array({Json{{"key", base64_encode("/wrong/key")},
                                {"value", base64_encode("{}")},
                                {"mod_revision", "7"},
                                {"lease", "11"}}})}}
            .dump());
    EtcdAdmissionConsumptionStore store(options(), client);

    EXPECT_EQ(
        store.reserve(request("gateway-a/attempt-1")).status,
        AdmissionReserveStatus::Unavailable);
    EXPECT_FALSE(store.available());
}

TEST(EtcdAdmissionConsumptionStoreTest, InvalidLeaseGrantFailsClosed) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(Json::object().dump());
    client->enqueue(Json{{"ID", "0"}}.dump());
    EtcdAdmissionConsumptionStore store(options(), client);

    EXPECT_EQ(
        store.reserve(request("gateway-a/attempt-1")).status,
        AdmissionReserveStatus::Unavailable);
    EXPECT_FALSE(store.available());
    ASSERT_EQ(client->calls().size(), 2U);
    EXPECT_EQ(client->calls()[1].first, "/v3/lease/grant");
}

TEST(EtcdAdmissionConsumptionStoreTest, UnavailableTransportRemovesHealth) {
    auto client = std::make_shared<ScriptedEtcdClient>();
    client->enqueue(std::nullopt);
    EtcdAdmissionConsumptionStore store(options(), client);

    EXPECT_EQ(
        store.reserve(request("gateway-a/attempt-1")).status,
        AdmissionReserveStatus::Unavailable);
    EXPECT_FALSE(store.available());
}

}  // namespace
}  // namespace realm::game::gateway
