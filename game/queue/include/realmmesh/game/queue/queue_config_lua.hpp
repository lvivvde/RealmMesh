#pragma once

// Lua 解析入口(#126):只供解析实现与分层装载器显式选择；包含方须自行
// 链接 RealmMesh::Scripting。只用配置值的代码包含 queue_config.hpp。
#include "realmmesh/game/queue/queue_config.hpp"

#include <sol/forward.hpp>

namespace realm::game::queue {

/// 解析已合并的 Lua 根表；`queue` 节缺失或字段类型错抛
/// std::invalid_argument。
[[nodiscard]] QueueConfig parse_queue_config(const sol::table& root);

}  // namespace realm::game::queue
