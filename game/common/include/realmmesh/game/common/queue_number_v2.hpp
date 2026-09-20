#pragma once

#include "realmmesh/game/common/compact_jws.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace realm::game::common {

inline constexpr std::int64_t queue_number_v2_version = 2;
inline constexpr std::string_view queue_number_v2_issuer = "realmmesh/queue";
inline constexpr std::string_view queue_number_v2_audience =
    "realmmesh-queue";
inline constexpr std::string_view queue_number_v2_purpose =
    "queue-position";
inline constexpr std::chrono::seconds queue_number_v2_max_ttl{86'400};

struct QueueNumberV2SigningKey final {
    std::string kid;
    Ed25519Seed seed;
};

struct QueueNumberV2VerificationKey final {
    std::string kid;
    Ed25519PublicKey public_key;
};

struct QueueNumberV2Issue final {
    std::string identity_jti;
    std::uint64_t number{0};
    std::chrono::system_clock::time_point issued_at;
    std::chrono::system_clock::time_point identity_expires_at;
};

struct QueueNumberV2Claims final {
    std::string identity_jti;
    std::uint64_t number{0};
    std::chrono::system_clock::time_point issued_at;
    std::chrono::system_clock::time_point expires_at;
};

/// Queue Number v2 的唯一编解码模块。audience/purpose/version 固定在模块
/// 内部；调用方只能提供身份绑定、号码和时间。验证键按 kid 直接索引。
class QueueNumberV2Codec final {
public:
    QueueNumberV2Codec(
        QueueNumberV2SigningKey active_signing_key,
        std::vector<QueueNumberV2VerificationKey> verification_keys,
        std::chrono::seconds ttl);

    [[nodiscard]] std::string issue(const QueueNumberV2Issue& input) const;

    /// Queue Scheduler 的查询入口只携带 Queue Number；签名内的
    /// identity_jti/exp 是签发时已经绑定的事实。此重载验证完整 schema、
    /// 签名与自身时效并返回这些事实。
    [[nodiscard]] std::optional<QueueNumberV2Claims> validate(
        std::string_view token,
        std::chrono::system_clock::time_point now) const;

    /// 当调用方同时持有 Identity Token 时，再额外验证 jti 与身份到期
    /// 上界；Gateway 不使用本模块。
    [[nodiscard]] std::optional<QueueNumberV2Claims> validate(
        std::string_view token,
        std::string_view expected_identity_jti,
        std::chrono::system_clock::time_point identity_expires_at,
        std::chrono::system_clock::time_point now) const;

private:
    CompactJws signer_;
    std::string active_kid_;
    std::unordered_map<std::string, CompactJwsVerifier> verification_keys_;
    std::chrono::seconds ttl_;
};

}  // namespace realm::game::common
