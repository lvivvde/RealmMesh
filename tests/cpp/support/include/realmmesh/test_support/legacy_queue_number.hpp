#pragma once

// 测试专用夹具:已退役的 v1「admitted 排队号牌」编解码的保真副本。
//
// 存在的唯一理由:负例测试要能铸出一枚**旧世界的** admitted 排队号牌,再
// 断言生产系统拒绝它(网关不再接受排队号牌作准入凭据,见
// docs/adr/0009-identity-bound-admission-grant.md)。生产代码不得包含本头:
// 只要它能签发或解析 admitted 排队号牌,#82 的验收就破产。wire 格式与
// `game/common/src/queue_number.cpp`(#81 删除前)逐键一致,保真度由
// tests/cpp/game/common/queue_number_test.cpp 锁定。

#include "realmmesh/game/common/compact_jws.hpp"
#include "realmmesh/game/common/json.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace realm::test_support {

/// v1 排队号牌签发方(ADR-0006 原文);与身份 Token 的 iss 区分凭据类型。
inline constexpr std::string_view legacy_queue_number_issuer =
    "realmmesh/queue";

/// v1 排队号牌的 claims 集:号值 + admitted。admitted 自 ADR-0009 起不再
/// 具备任何准入效力,这里只为复现旧凭据。
struct LegacyQueueNumberClaims {
    std::uint64_t number;  // 号值,1 起
    bool admitted;  // 旧放行标记 = 重签 admitted=true
    std::chrono::system_clock::time_point issued_at;  // iat
    std::chrono::system_clock::time_point expires_at;  // exp
};

/// v1 排队号牌(EdDSA 紧凑 JWS)的编解码;行为等价于删除前的
/// `game::common::QueueNumberCodec`,仅命名空间与类名不同。
class LegacyQueueNumberCodec final {
public:
    LegacyQueueNumberCodec(
        realm::game::common::Ed25519Seed seed,
        std::string kid)
        : jws_(seed),
          kid_(std::move(kid)) {}

    /// 签发紧缩三段式 JWS;前提:claims 由调用方构造
    /// (number ≥ 1、expires_at 不早于 issued_at)。
    [[nodiscard]] std::string issue(const LegacyQueueNumberClaims& claims) const {
        const realm::game::common::JsonObject header{
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
        const realm::game::common::JsonObject payload{
            {"iss", std::string{legacy_queue_number_issuer}},
            {"number", static_cast<std::int64_t>(claims.number)},
            {"admitted", claims.admitted},
            {"iat", static_cast<std::int64_t>(issued_at)},
            {"exp", static_cast<std::int64_t>(expires_at)},
        };
        return jws_.encode(header, payload);
    }

    /// 验签在 claims 语义之前;头/载荷逐键核对,签名不合法即整体拒绝。
    /// number 为 0 视为非法(先例同 IdentityTokenCodec 的 account_id)。
    [[nodiscard]] std::optional<LegacyQueueNumberClaims> validate(
        std::string_view token,
        std::chrono::system_clock::time_point now) const {
        const auto payload = jws_.decode(token, kid_);
        if (!payload.has_value()) {
            return std::nullopt;
        }

        const auto* issuer =
            realm::game::common::json_string_member(*payload, "iss");
        const auto* number =
            realm::game::common::json_int_member(*payload, "number");
        const auto* admitted = bool_member(*payload, "admitted");
        const auto* issued_at =
            realm::game::common::json_int_member(*payload, "iat");
        const auto* expires_at =
            realm::game::common::json_int_member(*payload, "exp");
        if (payload->size() != 5 || issuer == nullptr || number == nullptr ||
            admitted == nullptr || issued_at == nullptr ||
            expires_at == nullptr) {
            return std::nullopt;
        }
        if (issuer->empty() || *issuer != legacy_queue_number_issuer ||
            *number <= 0) {
            return std::nullopt;  // 正 int64 号值必容于 uint64,无需上界检查。
        }

        const auto issued = std::chrono::system_clock::time_point(
            std::chrono::seconds{*issued_at});
        const auto expires = std::chrono::system_clock::time_point(
            std::chrono::seconds{*expires_at});
        if (now > expires + realm::game::common::jws_clock_leeway ||
            now + realm::game::common::jws_clock_leeway < issued) {
            return std::nullopt;
        }

        return LegacyQueueNumberClaims{
            .number = static_cast<std::uint64_t>(*number),
            .admitted = *admitted,
            .issued_at = issued,
            .expires_at = expires,
        };
    }

private:
    [[nodiscard]] static const bool* bool_member(
        const realm::game::common::JsonObject& object,
        std::string_view key) {
        const auto found = object.find(std::string{key});
        if (found == object.end()) {
            return nullptr;
        }
        return std::get_if<bool>(&found->second);
    }

    realm::game::common::CompactJws jws_;
    std::string kid_;
};

}  // namespace realm::test_support
