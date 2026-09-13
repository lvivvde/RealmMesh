#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace realm::loadgen {

/// 机器人失败分型(spec Testing Decisions:失败分型单测覆盖);按相位的
/// HTTP/帧层拒绝与网络层故障、超时分开,进报告的失败分解。
enum class FailureKind {
    None,
    /// verify 非 200 或响应缺 identity_token。
    VerifyRejected,
    /// 取号非 202 或响应缺号牌。
    TicketsRejected,
    /// progress / tickets/me 请求失败或响应畸形。
    PollFailed,
    /// 截止前未放行。
    AdmitTimeout,
    /// 网关以 EdgeError 拒绝 attach。
    AttachRejected,
    /// 未在时限内收到 attach 受理(1302)。
    AttachTimeout,
    /// 受理后未在时限内收到 EnterRealmGranted(1303)。
    HandoffTimeout,
    /// 交付相位收到坏帧或被 EdgeError 拒绝。
    HandoffRejected,
    /// TLS/网络层失败(拨号、握手、读写、解析)。
    ConnectionError,
};

[[nodiscard]] std::string_view failure_kind_name(FailureKind kind);

/// 单相时延样本(毫秒);分位用就近秩(最近样本序),max 直取。
class LatencyRecorder final {
public:
    void record(double milliseconds);

    [[nodiscard]] double percentile(double p) const;
    [[nodiscard]] double max() const;
    [[nodiscard]] std::uint64_t samples() const;

    struct Summary final {
        std::uint64_t samples{0};
        double p50_ms{0};
        double p99_ms{0};
        double max_ms{0};
    };
    [[nodiscard]] Summary summary() const;

    /// 归并另一记录器的样本(线程本地累计的合并路径)。
    void merge(const LatencyRecorder& other);

private:
    std::vector<double> samples_;
};

/// 相位计数:尝试/失败/按时分型/时延。线程本地累计后归并(无锁热点)。
struct PhaseCounters final {
    std::uint64_t attempts{0};
    std::uint64_t failures{0};
    std::map<FailureKind, std::uint64_t> by_kind;
    LatencyRecorder latency;

    void record_success(double milliseconds);
    void record_failure(FailureKind kind, double milliseconds);
    void merge(const PhaseCounters& other);

    /// 最近一次失败的分型(record_success 不清零):机器人终态失败
    /// 直接取这里,避免外层把网络层故障错记成相位拒绝。
    FailureKind last_failure{FailureKind::None};
};

/// 服务端地址(回环压测语境:host 为点分 IPv4 或主机名)。
struct ServiceAddress final {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
};

}  // namespace realm::loadgen
