#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>

namespace realm::game::realm {

/// Realm 专属配置节(根表 `realm`);宿主级节段仍由
/// parse_gateway_config 承载。
struct RealmConfig {
    /// 训练规则脚本;相对路径由分层加载器按 config_root 解析。必填。
    std::filesystem::path training_rule_file;
    /// 角色数据访问的工作线程数与在途上限(排队 + 运行中 + 未取走)。
    std::size_t data_workers{4};
    std::size_t data_capacity{256};
    /// 单个会话最多的未决请求(在途 + 排队);超出回 429。
    std::size_t max_pending_per_session{16};
    /// 429 回包携带的 retry_after_seconds。
    std::chrono::seconds retry_after{1};
};

}  // namespace realm::game::realm
