#pragma once

#include "realmmesh/network/transport/transport_config.hpp"
#include "realmmesh/scripting/lua_runtime.hpp"

#include <chrono>
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
    /// 两种凭据角色绝不复用 kid 或密钥：Queue Number v2 仅用于排位，
    /// Admission Grant 才能进入 Gateway。
    std::string queue_number_kid{"queue-number-v2"};
    std::string admission_grant_kid{"admission-grant-v1"};
    std::string admission_grant_issuer{"realmmesh/queue"};
    std::string deployment_id{"development"};
    /// 身份 Token 验签:期望签发方与对应 kid(由健全服发布)。
    std::string identity_kid{"login-verify-v1"};
    std::string identity_issuer{"realmmesh/login-verify"};
    /// 放行阀门(§5.2):步长与批间隔(≥2s);额度读轮询间隔。
    std::uint64_t release_step{3000};
    std::chrono::milliseconds release_interval{2000};
    std::chrono::milliseconds budget_interval{1000};
    /// 排队中号牌时效;admit_grace 同时作为 release ledger 的 Grant 窗口
    /// (ADR-0009:号牌与准入凭据自 cutover 起彻底分离,号牌再无准入语义)。
    std::chrono::seconds queued_number_ttl{3600};
    std::chrono::seconds admit_grace{300};
    /// etcd 存取(§5.2 key 契约,与写侧 #43/#46 共享)。发号映射按身份
    /// Token 的真实过期时间租约回收，不另配进程内 TTL。
    std::string budget_prefix{"/realmmesh/budgets/service"};
    std::string snapshot_key{"/realmmesh/queue/snapshot"};
    std::string issuance_prefix{"/realmmesh/queue/issuance"};
    std::string etcd_endpoint{"http://127.0.0.1:2379"};
};

class QueueConfigLoader final {
public:
    /// 解析已合并的 Lua 根表(供分层加载器复用);`queue` 节缺失或
    /// 字段类型错抛 std::invalid_argument。
    [[nodiscard]] static QueueConfig parse(const sol::table& root);
};

}  // namespace realm::game::queue
