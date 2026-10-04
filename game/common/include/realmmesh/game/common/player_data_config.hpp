#pragma once

#include "realmmesh/game/common/player_data_store.hpp"

#include <filesystem>
#include <string_view>

namespace realm::game::common {

/// "interactive" | "minimum"；其他取值抛 std::invalid_argument。
[[nodiscard]] CredentialHashCost parse_credential_hash_cost(
    std::string_view value);

/// bootstrap 文件的相对路径按 config_root 解析。
void resolve_player_data_paths(
    PlayerDataConfig& config, const std::filesystem::path& config_root);

}  // namespace realm::game::common
