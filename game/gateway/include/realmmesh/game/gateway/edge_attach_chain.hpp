#pragma once

#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/queue_number.hpp"
#include "realmmesh/game/gateway/edge_session_pipeline.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace realm::game::gateway {

/// attach 校验链的裁决;映射 EdgeError.code(见 edge_protocol.hpp):
/// InvalidCredentials → 1001、InvalidNumber → 2001、OutOfBudget → 1004。
/// NotPending 为协议误用(未知会话或已迁离 pending),按 1001 拒绝,
/// 客户端路径是重开连接重走管线。
enum class EdgeAttachVerdict : std::uint8_t {
    Accepted,
    NotPending,
    OutOfBudget,
    InvalidCredentials,
    InvalidNumber,
};

/// pending → fetching 的三重闸(主 spec §5.3):身份 Token → admitted
/// 号牌 → jti 单次消费,全部通过才迁移并占用拉取额度。额度探针先于
/// jti 消费,满额拒绝不烧凭据;已在管会话不因额度波动被驱逐。
/// 单线程约定:与 EdgeSessionPipeline 同线程(网关业务帧)。
class EdgeAttachChain final {
public:
    EdgeAttachChain(
        const common::IdentityTokenCodec& identity_codec,
        const common::QueueNumberCodec& number_codec,
        EdgeSessionPipeline& pipeline,
        std::string_view identity_issuer);

    /// 处理一次凭据提交;Accepted 时会话已迁入 fetching。
    [[nodiscard]] EdgeAttachVerdict handle(
        EdgeSessionId session,
        std::string_view identity_token,
        std::string_view number_token,
        std::chrono::system_clock::time_point now);

    /// 最近一次 Accepted 的账号(sub);其余裁决后值不变。
    [[nodiscard]] std::uint64_t last_account_id() const noexcept {
        return last_account_id_;
    }

    /// jti 重放拒绝累计(单调;#47 帧尾轮询发布
    /// edge_jti_replay_rejected_total 用)。
    [[nodiscard]] std::uint64_t replay_rejections() const noexcept {
        return replay_rejections_;
    }

private:
    const common::IdentityTokenCodec* identity_codec_;
    const common::QueueNumberCodec* number_codec_;
    EdgeSessionPipeline* pipeline_;
    common::IdentityReplayGuard replay_guard_;
    std::string identity_issuer_;
    std::uint64_t last_account_id_{0};
    std::uint64_t replay_rejections_{0};
};

}  // namespace realm::game::gateway
