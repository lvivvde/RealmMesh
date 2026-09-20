#pragma once

#include "realmmesh/game/common/admission_grant.hpp"
#include "realmmesh/game/common/queue_number_v2.hpp"

#include <chrono>
#include <optional>
#include <vector>

namespace realm::game::common {

/// Queue Scheduler 的新凭据启动配置。活动签名键用 optional 表达配置
/// 缺失；每种角色只能有一个活动键，验证环可保留轮换重叠键。
struct QueueAdmissionSecurityConfig final {
    std::optional<QueueNumberV2SigningKey> active_queue_number_key;
    std::vector<QueueNumberV2VerificationKey> queue_number_verification_keys;
    std::optional<AdmissionGrantSigningKey> active_admission_grant_key;
    std::vector<AdmissionGrantVerificationKey>
        admission_grant_verification_keys;
    std::chrono::seconds queue_number_ttl{3600};
    AdmissionGrantPolicy admission_grant;

    /// 在任何监听器创建前调用。除各模块自身约束外，拒绝跨角色 kid
    /// 或公钥复用，避免 Queue Number 私钥获得 Gateway 准入能力。
    void validate() const;
};

}  // namespace realm::game::common
