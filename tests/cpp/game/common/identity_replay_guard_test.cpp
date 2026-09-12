#include "realmmesh/game/common/identity_token.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace realm::game::common {
namespace {

/// jti 单次消费(spec #43):网关入口对身份 Token 的唯一重放缝。
/// 先例同 TicketReplayGuard:键即凭据标识,条目随 exp 过期清理,
/// 有效性本身由编解码器把关,守卫只回答"是否首次见"。
TEST(IdentityReplayGuardTest, ConsumesOnceAndRejectsReplay) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(1'000s);
    IdentityClaims claims{
        .issuer = "realmmesh/login-verify",
        .account_id = 7,
        .jti = std::string(32, 'a'),
        .issued_at = now,
        .expires_at = now + 30min,
    };

    IdentityReplayGuard guard;
    EXPECT_TRUE(guard.consume(claims, now));
    EXPECT_FALSE(guard.consume(claims, now + 1s));
}

TEST(IdentityReplayGuardTest, DistinctJtiAreIndependent) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(2'000s);
    IdentityReplayGuard guard;
    const IdentityClaims first{
        .issuer = "realmmesh/login-verify",
        .account_id = 7,
        .jti = std::string(31, 'a') + "1",
        .issued_at = now,
        .expires_at = now + 30min,
    };
    const IdentityClaims second{
        .issuer = "realmmesh/login-verify",
        .account_id = 8,
        .jti = std::string(31, 'a') + "2",
        .issued_at = now,
        .expires_at = now + 30min,
    };

    EXPECT_TRUE(guard.consume(first, now));
    EXPECT_TRUE(guard.consume(second, now));
    EXPECT_FALSE(guard.consume(first, now));
}

TEST(IdentityReplayGuardTest, ExpiredEntriesAreEvictedNotReplayed) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(3'000s);
    IdentityReplayGuard guard;
    const IdentityClaims stale{
        .issuer = "realmmesh/login-verify",
        .account_id = 7,
        .jti = std::string(32, 'b'),
        .issued_at = now,
        .expires_at = now + 30min,
    };
    EXPECT_TRUE(guard.consume(stale, now));

    // 有效窗口过后,同一 jti 重现视为新条目(过期拒绝是编解码器的职责)。
    const IdentityClaims renewed = stale;
    const auto later = now + 31min;
    EXPECT_TRUE(guard.consume(renewed, later));
}

TEST(IdentityReplayGuardTest, ReplayWithinLeewayAfterExpiryIsRejected) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(4'000s);
    IdentityReplayGuard guard;
    const IdentityClaims claims{
        .issuer = "realmmesh/login-verify",
        .account_id = 7,
        .jti = std::string(32, 'c'),
        .issued_at = now,
        .expires_at = now + 30min,
    };
    ASSERT_TRUE(guard.consume(claims, now));

    // exp 已过但仍在编解码器 leeway 窗口内,token 仍可能被验讫:
    // 条目必须存活到 exp+leeway,窗口内重放一律拒绝。
    const auto within_leeway = claims.expires_at + jws_clock_leeway - 1s;
    EXPECT_FALSE(guard.consume(claims, within_leeway));

    // leeway 窗口过后条目才清出,重现视为新条目(过期拒绝由编解码器负责)。
    const auto after_leeway = claims.expires_at + jws_clock_leeway + 1s;
    EXPECT_TRUE(guard.consume(claims, after_leeway));
}

}  // namespace
}  // namespace realm::game::common
