#include "realmmesh/client/adaptive_poller.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace realm::client {
namespace {

using std::chrono::milliseconds;

/// 抖动注入:按给定序列依次返回(用尽后重复最后一个)。
[[nodiscard]] JitterSource scripted_jitter(std::vector<double> values) {
    auto index = std::make_shared<std::size_t>(0);
    return [values = std::move(values), index] {
        const auto value = values[*index < values.size() ? *index
                                                         : values.size() - 1];
        ++*index;
        return value;
    };
}

[[nodiscard]] PollTierConfig spec_defaults() {
    PollTierConfig config;
    config.jitter_ratio = 0.0;
    return config;
}

/// 位次分档(spec §7 初值):>1000 → 5s;≤10 → 1s;其余 2s。
TEST(AdaptivePollerTest, TiersFollowSpecThresholds) {
    AdaptivePoller poller(spec_defaults());

    EXPECT_EQ(poller.next_interval(0), milliseconds{1000});
    EXPECT_EQ(poller.next_interval(10), milliseconds{1000});
    EXPECT_EQ(poller.next_interval(11), milliseconds{2000});
    EXPECT_EQ(poller.next_interval(1000), milliseconds{2000});
    EXPECT_EQ(poller.next_interval(1001), milliseconds{5000});
    EXPECT_EQ(poller.next_interval(50000), milliseconds{5000});
}

/// ±20% 抖动:比额定值低/高各 20%;min_interval 是硬下限(1s 缓存口径),
/// 抖动不得把它顶破。
TEST(AdaptivePollerTest, JitterStaysWithinTwentyPercentAndHonoursFloor) {
    // 缺省分档配置(抖动比 0.2、硬下限 1000ms)。
    AdaptivePoller poller(PollTierConfig{},
                          scripted_jitter({0.0, 1.0, 0.5, 0.0, 1.0, 1.0}));

    // far 档 5000ms:800 抖动比对应 5000*0.8/1.2。
    EXPECT_EQ(poller.next_interval(5000), milliseconds{4000});
    EXPECT_EQ(poller.next_interval(5000), milliseconds{6000});
    EXPECT_EQ(poller.next_interval(5000), milliseconds{5000});

    // near 档 1000ms:下抖到 800 被 min_interval(1000)顶回;
    // 上抖到 1200 仍是合法结果。
    EXPECT_EQ(poller.next_interval(5), milliseconds{1000});
    EXPECT_EQ(poller.next_interval(5), milliseconds{1200});
}

/// 连续失败到阈值起指数退避、30s 封顶;成功即复位(spec §7)。
TEST(AdaptivePollerTest, BackoffDoublesAfterThresholdAndCaps) {
    AdaptivePoller poller(spec_defaults());

    // 阈值前两次失败不影响节奏。
    poller.record_failure();
    poller.record_failure();
    EXPECT_EQ(poller.consecutive_failures(), 2U);
    EXPECT_EQ(poller.next_interval(5), milliseconds{1000});

    poller.record_failure();
    EXPECT_EQ(poller.next_interval(5), milliseconds{2000});
    poller.record_failure();
    EXPECT_EQ(poller.next_interval(5), milliseconds{4000});

    for (int step = 0; step < 6; ++step) {
        poller.record_failure();
    }
    EXPECT_EQ(poller.next_interval(5), milliseconds{30000});
    EXPECT_EQ(poller.next_interval(50000), milliseconds{30000});

    poller.record_success();
    EXPECT_EQ(poller.consecutive_failures(), 0U);
    EXPECT_EQ(poller.next_interval(5), milliseconds{1000});
}

/// ETA 本地插值(spec §7、ADR-0006):位次 / 放行速率;速率未知则不给估计。
TEST(AdaptivePollerTest, EstimatesEtaFromPositionOverRate) {
    EXPECT_EQ(AdaptivePoller::estimate_eta(100, 10.0),
              std::chrono::seconds{10});
    EXPECT_EQ(AdaptivePoller::estimate_eta(7, 2.5),
              std::chrono::seconds{2});
    EXPECT_EQ(AdaptivePoller::estimate_eta(0, 10.0), std::chrono::seconds{0});
    EXPECT_EQ(AdaptivePoller::estimate_eta(100, 0.0), std::nullopt);
    EXPECT_EQ(AdaptivePoller::estimate_eta(100, -1.0), std::nullopt);
}

/// 缺省抖动源可用:多次取值落在 [0,1) 且不恒等(生产路径不是常量)。
TEST(AdaptivePollerTest, DefaultJitterSourceVariesWithinUnitInterval) {
    const auto jitter = make_random_jitter_source();
    bool varies = false;
    const auto first = jitter();
    for (int index = 0; index < 64; ++index) {
        const auto value = jitter();
        EXPECT_GE(value, 0.0);
        EXPECT_LT(value, 1.0);
        varies = varies || value != first;
    }
    EXPECT_TRUE(varies);
}

}  // namespace
}  // namespace realm::client
