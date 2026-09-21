#include "realmmesh/game/gateway/admission_consumption_store.hpp"

#include "realmmesh/game/common/compact_jws.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace realm::game::gateway {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::system_clock;

constexpr std::size_t jti_size = 32;
constexpr std::size_t digest_size = 32;
constexpr std::size_t max_owner_size = 128;
constexpr std::size_t max_cas_attempts = 8;
constexpr std::chrono::seconds max_reservation_ttl{300};

enum class StoredState : std::uint8_t {
    Reserved,
    Consumed,
};

struct StoredRecord final {
    StoredState state{StoredState::Reserved};
    std::string grant_digest;
    std::string owner;
    std::uint64_t fencing{0};
    Clock::time_point lease_expires_at;
    Clock::time_point consume_until;
};

struct LoadedRecord final {
    std::optional<StoredRecord> record;
    std::int64_t mod_revision{0};
    std::int64_t lease_id{0};
};

bool valid_lower_hex(std::string_view value, std::size_t size) {
    return value.size() == size &&
        std::ranges::all_of(value, [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

bool valid_jti(std::string_view value) {
    return valid_lower_hex(value, jti_size);
}

bool valid_owner(std::string_view value) {
    return !value.empty() && value.size() <= max_owner_size &&
        std::ranges::all_of(value, [](char character) {
            return character >= '!' && character <= '~';
        });
}

Clock::time_point normalized(Clock::time_point value) {
    return Clock::time_point{std::chrono::duration_cast<std::chrono::seconds>(
        value.time_since_epoch())};
}

std::int64_t epoch_seconds(Clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               value.time_since_epoch())
        .count();
}

bool valid_time(Clock::time_point value) {
    return epoch_seconds(normalized(value)) >= 0;
}

void validate_request(const AdmissionReserveRequest& request) {
    if (!valid_jti(request.identity_jti) || !valid_jti(request.grant_jti) ||
        !valid_owner(request.owner) || !valid_time(request.now) ||
        !valid_time(request.consume_until) ||
        normalized(request.consume_until) < normalized(request.now)) {
        throw std::invalid_argument("invalid admission reservation request");
    }
}

void validate_reservation(const AdmissionReservation& reservation) {
    if (!valid_jti(reservation.identity_jti) ||
        !valid_jti(reservation.grant_jti) ||
        !valid_owner(reservation.owner) || reservation.fencing == 0 ||
        !valid_time(reservation.lease_expires_at) ||
        !valid_time(reservation.consume_until)) {
        throw std::invalid_argument("invalid admission reservation");
    }
}

std::string stable_digest(
    std::string_view value, const AdmissionConsumptionDigestKey& key) {
    if (sodium_init() < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
    std::array<unsigned char, digest_size> digest{};
    if (crypto_generichash(
            digest.data(), digest.size(),
            reinterpret_cast<const unsigned char*>(value.data()),
            value.size(),
            reinterpret_cast<const unsigned char*>(key.data()),
            key.size()) != 0) {
        throw std::runtime_error("failed to derive admission identifier digest");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 0x0FU]);
    }
    return result;
}

AdmissionReservation reservation_from(
    const AdmissionReserveRequest& request,
    const StoredRecord& record) {
    return {
        .identity_jti = request.identity_jti,
        .grant_jti = request.grant_jti,
        .owner = request.owner,
        .fencing = record.fencing,
        .lease_expires_at = record.lease_expires_at,
        .consume_until = record.consume_until,
    };
}

bool matches(
    const StoredRecord& record,
    const AdmissionReservation& reservation,
    std::string_view grant_digest) {
    return record.fencing == reservation.fencing &&
        record.owner == reservation.owner &&
        record.grant_digest == grant_digest;
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
        const bool final_block = offset + 4U == input.size();
        const bool third_padding = input[offset + 2U] == '=';
        const bool fourth_padding = input[offset + 3U] == '=';
        if ((!final_block && (third_padding || fourth_padding)) ||
            (third_padding && !fourth_padding)) {
            return std::nullopt;
        }
        const int first = decode(input[offset]);
        const int second = decode(input[offset + 1U]);
        const int third = third_padding ? 0 : decode(input[offset + 2U]);
        const int fourth = fourth_padding ? 0 : decode(input[offset + 3U]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0) {
            return std::nullopt;
        }
        if ((third_padding && (second & 0x0F) != 0) ||
            (fourth_padding && !third_padding && (third & 0x03) != 0)) {
            return std::nullopt;
        }
        const auto value = (static_cast<std::uint32_t>(first) << 18U) |
                           (static_cast<std::uint32_t>(second) << 12U) |
                           (static_cast<std::uint32_t>(third) << 6U) |
                           static_cast<std::uint32_t>(fourth);
        output.push_back(static_cast<char>((value >> 16U) & 0xFFU));
        if (!third_padding) {
            output.push_back(static_cast<char>((value >> 8U) & 0xFFU));
        }
        if (!fourth_padding) {
            output.push_back(static_cast<char>(value & 0xFFU));
        }
    }
    return output;
}

std::optional<std::uint64_t> unsigned_integer(
    const Json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end()) return std::nullopt;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    if (!found->is_string()) return std::nullopt;
    const auto text = found->get<std::string>();
    if (text.empty()) return std::nullopt;
    std::uint64_t result = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') return std::nullopt;
        const auto next = static_cast<std::uint64_t>(digit - '0');
        if (result >
            (std::numeric_limits<std::uint64_t>::max() - next) / 10U) {
            return std::nullopt;
        }
        result = result * 10U + next;
    }
    return result;
}

std::optional<std::int64_t> nonnegative_integer(
    const Json& value, const char* key) {
    const auto parsed = unsigned_integer(value, key);
    if (!parsed.has_value() ||
        *parsed >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*parsed);
}

std::optional<std::chrono::seconds> retention_ttl(
    Clock::time_point now, Clock::time_point consume_until) {
    const auto count =
        std::chrono::duration_cast<std::chrono::seconds>(
            normalized(consume_until) - normalized(now))
            .count();
    if (count < 0 || count == std::numeric_limits<std::int64_t>::max()) {
        return std::nullopt;
    }
    // consume_until 是含端点；多保留一秒，避免 lease 在仍可验证的最后
    // 一秒提前删除 Consumed。
    return std::chrono::seconds{count + 1};
}

Json encode_record(const StoredRecord& record) {
    return {
        {"state",
         record.state == StoredState::Reserved ? "reserved" : "consumed"},
        {"grant", record.grant_digest},
        {"owner", record.owner},
        {"fencing", record.fencing},
        {"lease_expires_at", epoch_seconds(record.lease_expires_at)},
        {"consume_until", epoch_seconds(record.consume_until)},
    };
}

std::optional<StoredRecord> decode_record(std::string_view text) {
    Json value;
    try {
        value = Json::parse(text);
    } catch (const Json::exception&) {
        return std::nullopt;
    }
    if (!value.is_object() || value.size() != 6U) return std::nullopt;
    const auto state = value.find("state");
    const auto grant = value.find("grant");
    const auto owner = value.find("owner");
    const auto fencing = unsigned_integer(value, "fencing");
    const auto lease = nonnegative_integer(value, "lease_expires_at");
    const auto consume = nonnegative_integer(value, "consume_until");
    if (state == value.end() || !state->is_string() ||
        grant == value.end() || !grant->is_string() ||
        owner == value.end() || !owner->is_string() ||
        !fencing.has_value() || *fencing == 0 || !lease.has_value() ||
        !consume.has_value()) {
        return std::nullopt;
    }
    const auto state_text = state->get<std::string>();
    StoredState parsed_state;
    if (state_text == "reserved") {
        parsed_state = StoredState::Reserved;
    } else if (state_text == "consumed") {
        parsed_state = StoredState::Consumed;
    } else {
        return std::nullopt;
    }
    const auto grant_text = grant->get<std::string>();
    const auto owner_text = owner->get<std::string>();
    if (!valid_lower_hex(grant_text, digest_size * 2U) ||
        !valid_owner(owner_text)) {
        return std::nullopt;
    }
    return StoredRecord{
        .state = parsed_state,
        .grant_digest = grant_text,
        .owner = owner_text,
        .fencing = *fencing,
        .lease_expires_at = Clock::time_point{std::chrono::seconds{*lease}},
        .consume_until = Clock::time_point{std::chrono::seconds{*consume}},
    };
}

}  // namespace

AdmissionConsumptionDigestKey parse_admission_consumption_digest_key(
    std::string_view value) {
    return common::parse_identity_seed_hex(value);
}

void AdmissionConsumptionOptions::validate() const {
    if (key_prefix.empty() || key_prefix.front() != '/' ||
        key_prefix.back() == '/' || key_prefix.size() > 256U ||
        key_prefix.find("//") != std::string::npos ||
        !std::ranges::all_of(key_prefix, [](char character) {
            return (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '/' ||
                character == '-' || character == '_' || character == '.';
        })) {
        throw std::invalid_argument("invalid admission consumption key prefix");
    }
    if (reservation_ttl <= std::chrono::seconds::zero() ||
        reservation_ttl > max_reservation_ttl) {
        throw std::invalid_argument("invalid admission reservation ttl");
    }
    if (std::ranges::all_of(digest_key, [](std::byte value) {
            return value == std::byte{0};
        })) {
        throw std::invalid_argument(
            "admission consumption digest key must not be all zero");
    }
}

class InMemoryAdmissionConsumptionStore::Impl final {
public:
    explicit Impl(AdmissionConsumptionOptions options)
        : options_(std::move(options)) {
        options_.validate();
    }

    AdmissionReserveResult reserve(const AdmissionReserveRequest& request) {
        validate_request(request);
        std::scoped_lock lock(mutex_);
        if (!available_) return {AdmissionReserveStatus::Unavailable, {}};
        const auto identity = stable_digest(request.identity_jti, options_.digest_key);
        const auto grant = stable_digest(request.grant_jti, options_.digest_key);
        const auto now = normalized(request.now);
        const auto consume_until = normalized(request.consume_until);
        auto found = records_.find(identity);
        if (found != records_.end()) {
            auto& record = found->second;
            if (record.state == StoredState::Consumed &&
                now <= record.consume_until) {
                return {AdmissionReserveStatus::Consumed, {}};
            }
            if (record.state == StoredState::Reserved &&
                now < record.lease_expires_at) {
                if (record.owner == request.owner &&
                    record.grant_digest == grant) {
                    return {
                        AdmissionReserveStatus::Reserved,
                        reservation_from(request, record),
                    };
                }
                return {AdmissionReserveStatus::InProgress, {}};
            }
            if (record.fencing == std::numeric_limits<std::uint64_t>::max()) {
                available_ = false;
                return {AdmissionReserveStatus::Unavailable, {}};
            }
            ++record.fencing;
            record.state = StoredState::Reserved;
            record.grant_digest = grant;
            record.owner = request.owner;
            record.lease_expires_at = now + options_.reservation_ttl;
            record.consume_until = consume_until;
            return {
                AdmissionReserveStatus::Reserved,
                reservation_from(request, record),
            };
        }

        StoredRecord record{
            .state = StoredState::Reserved,
            .grant_digest = grant,
            .owner = request.owner,
            .fencing = 1,
            .lease_expires_at = now + options_.reservation_ttl,
            .consume_until = consume_until,
        };
        const auto [inserted, ok] = records_.emplace(identity, record);
        static_cast<void>(ok);
        return {
            AdmissionReserveStatus::Reserved,
            reservation_from(request, inserted->second),
        };
    }

    AdmissionMutationStatus commit(
        const AdmissionReservation& reservation, Clock::time_point now) {
        validate_reservation(reservation);
        if (!valid_time(now)) throw std::invalid_argument("invalid commit time");
        std::scoped_lock lock(mutex_);
        if (!available_) return AdmissionMutationStatus::Unavailable;
        const auto identity =
            stable_digest(reservation.identity_jti, options_.digest_key);
        const auto grant =
            stable_digest(reservation.grant_jti, options_.digest_key);
        const auto found = records_.find(identity);
        if (found == records_.end()) return AdmissionMutationStatus::Lost;
        auto& record = found->second;
        if (record.state == StoredState::Consumed) {
            return record.fencing == reservation.fencing &&
                    record.grant_digest == grant
                       ? AdmissionMutationStatus::Applied
                       : AdmissionMutationStatus::Lost;
        }
        const auto current_time = normalized(now);
        if (!matches(record, reservation, grant) ||
            current_time >= record.lease_expires_at ||
            current_time > record.consume_until) {
            return AdmissionMutationStatus::Lost;
        }
        record.state = StoredState::Consumed;
        record.consume_until = normalized(reservation.consume_until);
        return AdmissionMutationStatus::Applied;
    }

    AdmissionMutationStatus release(
        const AdmissionReservation& reservation, Clock::time_point now) {
        validate_reservation(reservation);
        if (!valid_time(now)) throw std::invalid_argument("invalid release time");
        std::scoped_lock lock(mutex_);
        if (!available_) return AdmissionMutationStatus::Unavailable;
        const auto identity =
            stable_digest(reservation.identity_jti, options_.digest_key);
        const auto grant =
            stable_digest(reservation.grant_jti, options_.digest_key);
        const auto found = records_.find(identity);
        if (found == records_.end() ||
            found->second.state != StoredState::Reserved ||
            !matches(found->second, reservation, grant)) {
            return AdmissionMutationStatus::Lost;
        }
        found->second.lease_expires_at = normalized(now);
        return AdmissionMutationStatus::Applied;
    }

    AdmissionConsumptionOptions options_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, StoredRecord> records_;
    std::atomic_bool available_{true};
};

InMemoryAdmissionConsumptionStore::InMemoryAdmissionConsumptionStore(
    AdmissionConsumptionOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

InMemoryAdmissionConsumptionStore::~InMemoryAdmissionConsumptionStore() =
    default;

AdmissionReserveResult InMemoryAdmissionConsumptionStore::reserve(
    const AdmissionReserveRequest& request) {
    return impl_->reserve(request);
}

AdmissionMutationStatus InMemoryAdmissionConsumptionStore::commit(
    const AdmissionReservation& reservation, Clock::time_point now) {
    return impl_->commit(reservation, now);
}

AdmissionMutationStatus InMemoryAdmissionConsumptionStore::release(
    const AdmissionReservation& reservation, Clock::time_point now) {
    return impl_->release(reservation, now);
}

bool InMemoryAdmissionConsumptionStore::available() const noexcept {
    return impl_->available_.load();
}

bool InMemoryAdmissionConsumptionStore::probe() {
    return available();
}

void InMemoryAdmissionConsumptionStore::set_available(bool value) noexcept {
    impl_->available_.store(value);
}

class EtcdAdmissionConsumptionStore::Impl final {
public:
    Impl(
        AdmissionConsumptionOptions options,
        std::shared_ptr<cluster::IEtcdHttpClient> client)
        : options_(std::move(options)), client_(std::move(client)) {
        options_.validate();
        if (client_ == nullptr) {
            throw std::invalid_argument("admission etcd client must not be null");
        }
    }

    AdmissionReserveResult reserve(const AdmissionReserveRequest& request) {
        validate_request(request);
        const auto identity = stable_digest(request.identity_jti, options_.digest_key);
        const auto grant = stable_digest(request.grant_jti, options_.digest_key);
        const auto key = options_.key_prefix + "/" + identity;
        const auto now = normalized(request.now);
        std::optional<std::int64_t> replacement_lease;
        for (std::size_t attempt = 0; attempt < max_cas_attempts; ++attempt) {
            const auto loaded = load(key);
            if (!loaded.has_value()) return unavailable_reserve();
            std::uint64_t fencing = 1;
            if (loaded->record.has_value()) {
                const auto& current = *loaded->record;
                if (current.state == StoredState::Consumed &&
                    now <= current.consume_until) {
                    available_.store(true);
                    return {AdmissionReserveStatus::Consumed, {}};
                }
                if (current.state == StoredState::Reserved &&
                    now < current.lease_expires_at) {
                    available_.store(true);
                    if (current.owner == request.owner &&
                        current.grant_digest == grant) {
                        return {
                            AdmissionReserveStatus::Reserved,
                            reservation_from(request, current),
                        };
                    }
                    return {AdmissionReserveStatus::InProgress, {}};
                }
                if (current.fencing ==
                    std::numeric_limits<std::uint64_t>::max()) {
                    return unavailable_reserve();
                }
                fencing = current.fencing + 1U;
            }
            StoredRecord replacement{
                .state = StoredState::Reserved,
                .grant_digest = grant,
                .owner = request.owner,
                .fencing = fencing,
                .lease_expires_at = now + options_.reservation_ttl,
                .consume_until = normalized(request.consume_until),
            };
            if (!replacement_lease.has_value()) {
                const auto ttl = retention_ttl(now, request.consume_until);
                if (!ttl.has_value()) return unavailable_reserve();
                replacement_lease = grant_lease(*ttl);
                if (!replacement_lease.has_value()) {
                    return unavailable_reserve();
                }
            }
            const auto written = compare_and_put(
                key, *loaded, replacement, *replacement_lease);
            if (!written.has_value()) return unavailable_reserve();
            if (!*written) continue;
            available_.store(true);
            return {
                AdmissionReserveStatus::Reserved,
                reservation_from(request, replacement),
            };
        }
        return unavailable_reserve();
    }

    AdmissionMutationStatus commit(
        const AdmissionReservation& reservation, Clock::time_point now) {
        return mutate(reservation, now, true);
    }

    AdmissionMutationStatus release(
        const AdmissionReservation& reservation, Clock::time_point now) {
        return mutate(reservation, now, false);
    }

    bool available() const noexcept { return available_.load(); }

    bool probe() {
        const Json request{
            {"key", base64_encode(options_.key_prefix)}, {"limit", "1"}};
        const auto body = client_->post(
            "/v3/kv/range", request.dump(), nullptr);
        if (!body.has_value()) {
            available_.store(false);
            return false;
        }
        try {
            const auto response = Json::parse(*body);
            if (!response.is_object() || response.contains("error")) {
                available_.store(false);
                return false;
            }
            available_.store(true);
            return true;
        } catch (const Json::exception&) {
            available_.store(false);
            return false;
        }
    }

private:
    std::optional<LoadedRecord> load(const std::string& key) {
        const Json request{{"key", base64_encode(key)}};
        const auto body = client_->post(
            "/v3/kv/range", request.dump(), nullptr);
        if (!body.has_value()) return std::nullopt;
        try {
            const auto response = Json::parse(*body);
            const auto entries = response.find("kvs");
            if (entries == response.end()) return LoadedRecord{};
            if (!entries->is_array() || entries->size() != 1U) {
                return std::nullopt;
            }
            const auto& entry = entries->front();
            const auto returned_key = base64_decode(entry.value("key", ""));
            const auto value = base64_decode(entry.value("value", ""));
            const auto revision = nonnegative_integer(entry, "mod_revision");
            const auto lease = nonnegative_integer(entry, "lease");
            if (!returned_key.has_value() || *returned_key != key ||
                !value.has_value() || !revision.has_value() ||
                *revision == 0 || !lease.has_value() || *lease == 0) {
                return std::nullopt;
            }
            const auto record = decode_record(*value);
            if (!record.has_value()) return std::nullopt;
            return LoadedRecord{
                .record = std::move(*record),
                .mod_revision = *revision,
                .lease_id = *lease,
            };
        } catch (const Json::exception&) {
            return std::nullopt;
        }
    }

    std::optional<bool> compare_and_put(
        const std::string& key,
        const LoadedRecord& loaded,
        const StoredRecord& replacement,
        std::int64_t lease_id) {
        Json compare{
            {"key", base64_encode(key)},
            {"result", "EQUAL"},
        };
        if (loaded.record.has_value()) {
            compare["target"] = "MOD";
            compare["mod_revision"] = loaded.mod_revision;
        } else {
            compare["target"] = "VERSION";
            compare["version"] = 0;
        }
        const auto encoded = encode_record(replacement).dump();
        const Json transaction{
            {"compare", Json::array({std::move(compare)})},
            {"success",
             Json::array({Json{{"requestPut",
                                {{"key", base64_encode(key)},
                                 {"value", base64_encode(encoded)},
                                 {"lease", std::to_string(lease_id)}}}}})},
        };
        const auto body = client_->post(
            "/v3/kv/txn", transaction.dump(), nullptr);
        if (!body.has_value()) return std::nullopt;
        try {
            const auto response = Json::parse(*body);
            const auto succeeded = response.find("succeeded");
            if (succeeded == response.end() || !succeeded->is_boolean()) {
                return std::nullopt;
            }
            return succeeded->get<bool>();
        } catch (const Json::exception&) {
            return std::nullopt;
        }
    }

    std::optional<std::int64_t> grant_lease(std::chrono::seconds ttl) {
        const Json request{{"TTL", std::to_string(ttl.count())}};
        const auto body = client_->post(
            "/v3/lease/grant", request.dump(), nullptr);
        if (!body.has_value()) return std::nullopt;
        try {
            const auto response = Json::parse(*body);
            const auto id = nonnegative_integer(response, "ID");
            if (!id.has_value() || *id == 0) return std::nullopt;
            return id;
        } catch (const Json::exception&) {
            return std::nullopt;
        }
    }

    AdmissionMutationStatus mutate(
        const AdmissionReservation& reservation,
        Clock::time_point now,
        bool commit) {
        validate_reservation(reservation);
        if (!valid_time(now)) {
            throw std::invalid_argument("invalid admission mutation time");
        }
        const auto identity =
            stable_digest(reservation.identity_jti, options_.digest_key);
        const auto grant =
            stable_digest(reservation.grant_jti, options_.digest_key);
        const auto key = options_.key_prefix + "/" + identity;
        const auto current_time = normalized(now);
        for (std::size_t attempt = 0; attempt < max_cas_attempts; ++attempt) {
            const auto loaded = load(key);
            if (!loaded.has_value()) return unavailable_mutation();
            if (!loaded->record.has_value()) {
                available_.store(true);
                return AdmissionMutationStatus::Lost;
            }
            auto replacement = *loaded->record;
            if (replacement.state == StoredState::Consumed) {
                available_.store(true);
                return commit && replacement.fencing == reservation.fencing &&
                        replacement.grant_digest == grant
                           ? AdmissionMutationStatus::Applied
                           : AdmissionMutationStatus::Lost;
            }
            if (!matches(replacement, reservation, grant)) {
                available_.store(true);
                return AdmissionMutationStatus::Lost;
            }
            if (commit &&
                (current_time >= replacement.lease_expires_at ||
                 current_time > replacement.consume_until)) {
                available_.store(true);
                return AdmissionMutationStatus::Lost;
            }
            if (commit) {
                replacement.state = StoredState::Consumed;
                replacement.consume_until = normalized(reservation.consume_until);
            } else {
                replacement.lease_expires_at = current_time;
            }
            const auto written = compare_and_put(
                key, *loaded, replacement, loaded->lease_id);
            if (!written.has_value()) return unavailable_mutation();
            if (!*written) continue;
            available_.store(true);
            return AdmissionMutationStatus::Applied;
        }
        return unavailable_mutation();
    }

    AdmissionReserveResult unavailable_reserve() {
        available_.store(false);
        return {AdmissionReserveStatus::Unavailable, {}};
    }

    AdmissionMutationStatus unavailable_mutation() {
        available_.store(false);
        return AdmissionMutationStatus::Unavailable;
    }

    AdmissionConsumptionOptions options_;
    std::shared_ptr<cluster::IEtcdHttpClient> client_;
    std::atomic_bool available_{true};
};

EtcdAdmissionConsumptionStore::EtcdAdmissionConsumptionStore(
    AdmissionConsumptionOptions options,
    std::shared_ptr<cluster::IEtcdHttpClient> client)
    : impl_(
          std::make_unique<Impl>(std::move(options), std::move(client))) {}

EtcdAdmissionConsumptionStore::~EtcdAdmissionConsumptionStore() = default;

AdmissionReserveResult EtcdAdmissionConsumptionStore::reserve(
    const AdmissionReserveRequest& request) {
    return impl_->reserve(request);
}

AdmissionMutationStatus EtcdAdmissionConsumptionStore::commit(
    const AdmissionReservation& reservation, Clock::time_point now) {
    return impl_->commit(reservation, now);
}

AdmissionMutationStatus EtcdAdmissionConsumptionStore::release(
    const AdmissionReservation& reservation, Clock::time_point now) {
    return impl_->release(reservation, now);
}

bool EtcdAdmissionConsumptionStore::available() const noexcept {
    return impl_->available();
}

bool EtcdAdmissionConsumptionStore::probe() { return impl_->probe(); }

}  // namespace realm::game::gateway
