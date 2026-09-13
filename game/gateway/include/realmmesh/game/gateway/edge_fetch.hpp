#pragma once

#include <chrono>
#include <cstdint>

namespace realm::game::gateway {

/// 一次拉取尝试的结果(#44):ok + 源自报耗时(延迟桩=注入时延,
/// 真 DB=往返耗时);调度器在 now+duration 处结算该次尝试。
struct EdgeFetchOutcome {
    bool ok{false};
    std::chrono::milliseconds duration{0};
};

/// 拉取源接口(spec #44):只表达"一次尝试",重试与退避归调度器,
/// 真实数据源替换(spec §12 TODO)不动调度器与阶段机。
class EdgeFetchSource {
public:
    virtual ~EdgeFetchSource() = default;
    [[nodiscard]] virtual EdgeFetchOutcome fetch(std::uint64_t account_id) = 0;
};

/// 默认延迟桩:恒成功、按时延自报耗时;真 DB 接入前的过渡形态。
/// 时延即规格的 fetch_latency_ms(默认 100),构造期注入、不进 lua。
class DelayedFetchSource final : public EdgeFetchSource {
public:
    explicit DelayedFetchSource(
        std::chrono::milliseconds latency = std::chrono::milliseconds{100})
        : latency_(latency) {}

    [[nodiscard]] EdgeFetchOutcome fetch(std::uint64_t) override {
        return {true, latency_};
    }

private:
    std::chrono::milliseconds latency_;
};

}  // namespace realm::game::gateway
