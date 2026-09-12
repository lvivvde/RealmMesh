#include "realmmesh/game/common/queue_number.hpp"

#include "realmmesh/game/common/json.hpp"

namespace realm::game::common {
namespace {

const bool* bool_member(const JsonObject& object, std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end()) {
        return nullptr;
    }
    return std::get_if<bool>(&found->second);
}

}  // namespace

QueueNumberCodec::QueueNumberCodec(Ed25519Seed seed, std::string kid)
    : jws_(seed),
      kid_(std::move(kid)) {}

std::string QueueNumberCodec::issue(const QueueNumberClaims& claims) const {
    const JsonObject header{
        {"alg", std::string{"EdDSA"}},
        {"typ", std::string{"JWT"}},
        {"kid", kid_},
    };
    const auto issued_at = std::chrono::duration_cast<std::chrono::seconds>(
                               claims.issued_at.time_since_epoch())
                               .count();
    const auto expires_at = std::chrono::duration_cast<std::chrono::seconds>(
                                claims.expires_at.time_since_epoch())
                                .count();
    const JsonObject payload{
        {"iss", std::string{queue_number_issuer}},
        {"number", static_cast<std::int64_t>(claims.number)},
        {"admitted", claims.admitted},
        {"iat", static_cast<std::int64_t>(issued_at)},
        {"exp", static_cast<std::int64_t>(expires_at)},
    };
    return jws_.encode(header, payload);
}

std::optional<QueueNumberClaims> QueueNumberCodec::validate(
    std::string_view token,
    std::chrono::system_clock::time_point now) const {
    const auto payload = jws_.decode(token, kid_);
    if (!payload.has_value()) {
        return std::nullopt;
    }

    const auto* issuer = json_string_member(*payload, "iss");
    const auto* number = json_int_member(*payload, "number");
    const auto* admitted = bool_member(*payload, "admitted");
    const auto* issued_at = json_int_member(*payload, "iat");
    const auto* expires_at = json_int_member(*payload, "exp");
    if (payload->size() != 5 || issuer == nullptr || number == nullptr ||
        admitted == nullptr || issued_at == nullptr ||
        expires_at == nullptr) {
        return std::nullopt;
    }
    if (issuer->empty() || *issuer != queue_number_issuer || *number <= 0) {
        return std::nullopt;  // 正 int64 号值必容于 uint64,无需上界检查。
    }

    const auto issued =
        std::chrono::system_clock::time_point(std::chrono::seconds{*issued_at});
    const auto expires = std::chrono::system_clock::time_point(
        std::chrono::seconds{*expires_at});
    if (now > expires + common::jws_clock_leeway ||
        now + common::jws_clock_leeway < issued) {
        return std::nullopt;
    }

    return QueueNumberClaims{
        .number = static_cast<std::uint64_t>(*number),
        .admitted = *admitted,
        .issued_at = issued,
        .expires_at = expires,
    };
}

}  // namespace realm::game::common
