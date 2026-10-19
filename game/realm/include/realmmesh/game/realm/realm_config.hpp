#pragma once

#include "realmmesh/game/common/player_data_config.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

namespace realm::game::realm {

/// Realm 服务配置:`realm` 节段加根表的帧参数与 player_data;宿主级节段
/// 由宿主分层装载解析为 HostConfig,接入面为 edge::EdgeConfig(ADR-0014)。
struct RealmConfig {
    struct SceneClient {
        bool enabled{false};
        std::string host{"127.0.0.1"};
        std::uint16_t port{7200};
        std::string instance_id{"scene-dev-01"};
        std::string server_identity{"scene-dev-01.scene.service"};
        std::string ca_file;
        std::string certificate_file;
        std::string private_key_file;
    } scene;
    /// 训练规则脚本;相对路径由分层加载器按 config_root 解析。必填。
    std::filesystem::path training_rule_file;
    /// 角色数据访问的工作线程数与在途上限(排队 + 运行中 + 未取走)。
    std::size_t data_workers{4};
    std::size_t data_capacity{256};
    /// 单个会话最多的未决请求(在途 + 排队);超出回 429。
    std::size_t max_pending_per_session{16};
    /// 429 回包携带的 retry_after_seconds。
    std::chrono::seconds retry_after{1};
    /// 根表 max_events_per_frame:每帧最多处理的接入面事件数。
    std::size_t max_events_per_frame{4'096};
    /// 根表 player_data:权威角色数据;Realm 装配要求 uri 非空。
    common::PlayerDataConfig player_data;
};

}  // namespace realm::game::realm
