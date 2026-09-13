#pragma once

#include <atomic>
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

/// 拨号失败分型计数(诊断口径):机器人的 ConnectionError 只说"网络层
/// 失败",细分计数把内核层/握手层原因带出来才可定位(CI runner 与本机
/// 网络栈差异大:拒连、端口不可用、握手中断是三种不同处置)。分型直接
/// 取自 network 客户端的 TlsDialFailure。inline 全局:进程内聚合,报告
/// 收尾读一次。
struct DialDiagnostics final {
    std::atomic<std::uint64_t> resolve{0};
    std::atomic<std::uint64_t> socket{0};
    std::atomic<std::uint64_t> configure{0};
    std::atomic<std::uint64_t> connect{0};
    std::atomic<std::uint64_t> connect_timeout{0};
    std::atomic<std::uint64_t> ssl_setup{0};
    std::atomic<std::uint64_t> handshake{0};
    std::atomic<std::uint64_t> cancelled{0};
    /// connect/timeout 失败路径最近一次的 errno(诊断补充线索)。
    std::atomic<int> last_errno{0};
};

inline DialDiagnostics dial_diagnostics;

/// 服务端地址(回环压测语境:host 为点分 IPv4 或主机名)。
struct ServiceAddress final {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
};

}  // namespace realm::loadgen
