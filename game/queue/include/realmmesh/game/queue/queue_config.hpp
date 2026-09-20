#pragma once

#include "realmmesh/network/transport/transport_config.hpp"
#include "realmmesh/scripting/lua_runtime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace realm::game::queue {

/// 排队调度服专属配置节(根表 `queue`);宿主级节段(logging、
/// service_discovery、metrics)仍由 GatewayConfigLoader::parse 承载。
/// TLS 路径支持配置直填或经环境变量名解析(先例同 login_verify)。
struct QueueConfig {
    network::TransportConfig::TlsServerIdentity tls;
    std::string listen_address{"127.0.0.1"};
    std::uint16_t listen_port{8444};
    /// 号牌签名键(kid 随 JWKS 语义单调;queue 不发布 JWKS,消费方
    /// 只有内网网关,静态载入)。
    std::string kid{"queue-v1"};
    /// 身份 Token 验签:期望签发方与对应 kid(由健全服发布)。
    std::string identity_kid{"login-verify-v1"};
    std::string identity_issuer{"realmmesh/login-verify"};
    /// 放行阀门(§5.2):步长与批间隔(≥2s);额度读轮询间隔。
    std::uint64_t release_step{3000};
    std::chrono::milliseconds release_interval{2000};
    std::chrono::milliseconds budget_interval{1000};
    /// 排队中号牌时效;admit_grace 在 #84 同时作为新 release ledger 的
    /// Grant 窗口，旧路由仍用它重签 admitted 号牌直至最终 cutover。
    std::chrono::seconds queued_number_ttl{3600};
    std::chrono::seconds admit_grace{300};
    /// 发号幂等映射:条目随身份 Token 过期,容量上限尽力保护。
    std::chrono::seconds idempotency_ttl{1800};
    std::size_t idempotency_capacity{1'000'000};
    /// etcd 存取(§5.2 key 契约,与写侧 #43/#46 共享)。
    std::string budget_prefix{"/realmmesh/budgets/service"};
    std::string snapshot_key{"/realmmesh/queue/snapshot"};
    std::string etcd_endpoint{"http://127.0.0.1:2379"};
    /// 冷备强校验(默认开):启动时快照不可读即失败——冷备未知时从零
    /// 重发会与存量号牌冲突(fail-closed)。无 etcd 的开发网状显式关闭。
    bool snapshot_required{true};
};

class QueueConfigLoader final {
public:
    /// 解析已合并的 Lua 根表(供分层加载器复用);`queue` 节缺失或
    /// 字段类型错抛 std::invalid_argument。
    [[nodiscard]] static QueueConfig parse(const sol::table& root);
};

}  // namespace realm::game::queue
