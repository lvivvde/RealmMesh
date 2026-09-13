#pragma once

#include <string_view>

namespace realm::client {

/// 客户端登录链路七态(spec §7 客户端契约;Idle 为回退起点/终点):
/// verifying → queued → admitted → gateway_connecting →
/// handoff_received → realm_connecting → in_game。
enum class LoginStage {
    Idle,
    Verifying,
    Queued,
    Admitted,
    GatewayConnecting,
    HandoffReceived,
    RealmConnecting,
    InGame,
};

[[nodiscard]] std::string_view login_stage_name(LoginStage stage) noexcept;

/// 客户端侧失败分型(口径对齐 #48 压测侧;独立枚举,不依赖 tools/)。
enum class ChainFailure {
    None,
    VerifyRejected,
    TicketRejected,
    ProgressFailed,
    AdmitTimeout,
    GatewayConnectFailed,
    AttachRejected,
    HandoffTimeout,
    /// 交付相位收到坏帧或被 EdgeError 拒绝。
    HandoffRejected,
    RealmConnectFailed,
    EnterRealmRejected,
    DeadlineExceeded,
};

[[nodiscard]] std::string_view chain_failure_name(
    ChainFailure failure) noexcept;

}  // namespace realm::client
