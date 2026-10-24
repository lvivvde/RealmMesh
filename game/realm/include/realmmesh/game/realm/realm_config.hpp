#pragma once

#include "realmmesh/game/common/player_data_config.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace realm::game::realm {

/// Realm 服务配置:`realm` 节段加根表的帧参数与 player_data;宿主级节段
/// 由宿主分层装载解析为 HostConfig,接入面为 edge::EdgeConfig(ADR-0014)。
struct RealmConfig {
    struct SceneTarget {
        std::uint64_t shard_id{2};
        std::string host{"127.0.0.1"};
        std::uint16_t port{7201};
        std::string instance_id{"scene-dev-02"};
        std::string server_identity{"scene-dev-02.scene.service"};
        std::int32_t entry_x{0};
        std::int32_t entry_y{0};
    };
    struct SceneClient {
        bool enabled{false};
        /// 动态分片承载 CAS 与排空；只使用已发现且认证就绪的实例。
        bool dynamic{false};
        std::string host{"127.0.0.1"};
        std::uint16_t port{7200};
        std::string instance_id{"scene-dev-01"};
        std::string server_identity{"scene-dev-01.scene.service"};
        std::string ca_file;
        std::string certificate_file;
        std::string private_key_file;
        /// 显式目标允许列表；主分片 1 沿用上方配置。S3 只允许增加分片 2。
        std::vector<SceneTarget> targets;
    } scene;
    struct ChatTarget {
        std::string instance_id{"chat-dev-01"};
        std::string server_identity{"chat-dev-01.chat.service"};
        std::string host{"127.0.0.1"};
        std::uint16_t port{7300};
        // 0 = Realm Channel，1/2 = 对应 Scene Channel。每频道仅一个承载。
        std::vector<std::uint64_t> channels{0, 1, 2};
    };
    struct ChatClient {
        bool enabled{false};
        std::string realm_instance_id{"realm-dev-01"};
        bool discovery_enabled{false};
        std::string discovery_endpoint, discovery_prefix;
        std::string ca_file, certificate_file, private_key_file;
        std::vector<ChatTarget> targets;
    } chat;
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
